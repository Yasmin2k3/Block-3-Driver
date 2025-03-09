#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/proc_fs.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/cdev.h>
#include <linux/usb.h>

#define DEVICE_NAME "wacom-tablet"
#define BUFFER_SIZE 1024
#define URB_BUFFER_SIZE 64

#define DEVICE_VENDOR_ID 0x056a
#define DEVICE_PRODUCT_ID 0x0357

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yasmin, David, Waleed and April");
MODULE_DESCRIPTION("Wacom tablet driver with URB-based asynchronous reads.");
MODULE_VERSION("1.0");

static int major_number;
static char buffer[BUFFER_SIZE];
static size_t buffer_data_size = 0;

// Input device pointer
static struct input_dev *tablet_input_dev = NULL;

// Proc file entry pointer
static struct proc_dir_entry *pentry;

// Device class and device pointers for /dev creation
static struct class *tabletClass = NULL;
static struct device *tabletDevice = NULL;

// Character device structure
struct device_data {
	struct cdev cdev;
};
static struct device_data dev_data;

// URB and transfer buffer for asynchronous reads
static struct urb *wacom_urb = NULL;
static unsigned char *wacom_buf = NULL;

// FILE OPERATIONS
static int device_open(struct inode *inode, struct file *file)
{
	printk(KERN_INFO "Device opened\n");
	return 0;
}

static int device_release(struct inode *inode, struct file *file)
{
	printk(KERN_INFO "Device released\n");
	return 0;
}

static ssize_t device_read(struct file *file, char __user *user_buffer,
                           size_t len, loff_t *offset)
{
	size_t bytes_to_read = min(len, buffer_data_size);
	if (bytes_to_read == 0)
		return 0;
	if (copy_to_user(user_buffer, buffer, bytes_to_read))
		return -EFAULT;
	printk(KERN_INFO "Device read %zu bytes\n", bytes_to_read);
	memmove(buffer, buffer + bytes_to_read, buffer_data_size - bytes_to_read);
	buffer_data_size -= bytes_to_read;
	return bytes_to_read;
}

static struct file_operations fops = {
	.open = device_open,
	.release = device_release,
	.read = device_read,
};

// INPUT DEVICE SETUP
static int wacom_input_event(struct input_dev *dev, unsigned int type,
                             unsigned int code, int value)
{
	printk(KERN_INFO "Event triggered: Type=%u, Code=%u, Value=%d\n", type, code, value);
	return 0;
}

static int register_input_device(void)
{
	int error;
	tablet_input_dev = input_allocate_device();
	if (!tablet_input_dev) {
		printk(KERN_ALERT "Failed to allocate input device\n");
		return -ENOMEM;
	}

	tablet_input_dev->name = "Wacom Tablet Input Device";
	tablet_input_dev->id.bustype = BUS_USB;
	tablet_input_dev->evbit[0] = BIT(EV_ABS) | BIT(EV_KEY);
	input_set_abs_params(tablet_input_dev, ABS_X, 0, 10000, 0, 0);
	input_set_abs_params(tablet_input_dev, ABS_Y, 0, 10000, 0, 0);
	input_set_abs_params(tablet_input_dev, ABS_PRESSURE, 0, 255, 0, 0);
	tablet_input_dev->event = wacom_input_event;

	error = input_register_device(tablet_input_dev);
	if (error) {
		printk(KERN_ALERT "Failed to register input device\n");
		input_free_device(tablet_input_dev);
		return error;
	}

	printk(KERN_INFO "Input device registered\n");
	return 0;
}

static void unregister_input_device(void)
{
	if (tablet_input_dev) {
		input_unregister_device(tablet_input_dev);
		input_free_device(tablet_input_dev);
		printk(KERN_INFO "Input device unregistered\n");
	}
}

// PROC FILE SETUP
static struct proc_ops pops = {};

static int init_proc(void)
{
	pentry = proc_create(DEVICE_NAME, 0644, NULL, &pops);
	if (!pentry) {
		printk(KERN_ALERT "Failed to create proc entry\n");
		return -EFAULT;
	}
	printk(KERN_INFO "Proc file created at /proc/%s\n", DEVICE_NAME);
	return 0;
}

static void exit_proc(void)
{
	proc_remove(pentry);
	printk(KERN_INFO "Proc file /proc/%s removed\n", DEVICE_NAME);
}

// URB CALLBACK: Called when data is received from the device.
static void wacom_usb_read_callback(struct urb *urb)
{
	int status = urb->status;

	if (status) {
		printk(KERN_INFO "URB read error: %d\n", status);
		return;
	}

	// Ensure we have enough data (assumed at least 3 bytes: X, Y, Pressure)
	if (urb->actual_length >= 3) {
		int x = wacom_buf[0];
		int y = wacom_buf[1];
		int pressure = wacom_buf[2];

		// Generate input events for the X, Y, and pressure data.
		input_event(tablet_input_dev, EV_ABS, ABS_X, x);
		input_event(tablet_input_dev, EV_ABS, ABS_Y, y);
		input_event(tablet_input_dev, EV_ABS, ABS_PRESSURE, pressure);
		input_sync(tablet_input_dev);

		// Optionally, write to an internal buffer for user-space reading.
		buffer_data_size += snprintf(buffer + buffer_data_size,
		                              BUFFER_SIZE - buffer_data_size,
		                              "X=%d, Y=%d, Pressure=%d\n", x, y, pressure);
	}

	// Resubmit the URB so we continue receiving data.
	usb_submit_urb(urb, GFP_ATOMIC);
}

// USB DEVICE TABLE
static struct usb_device_id my_usb_table[] = {
	{ USB_DEVICE(DEVICE_VENDOR_ID, DEVICE_PRODUCT_ID) },
	{},
};
MODULE_DEVICE_TABLE(usb, my_usb_table);

// PROBE FUNCTION: Called when the device is plugged in.
static int wacom_usb_probe(struct usb_interface *intf,
                           const struct usb_device_id *id)
{
	int chrdev_result, result;
	dev_t dev;
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	int pipe, interval;

	chrdev_result = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
	major_number = MAJOR(dev);
	if (major_number < 0) {
		printk(KERN_ALERT "Failed to register major number\n");
		return major_number;
	}
	printk(KERN_INFO "%s device registered with major number %d\n", DEVICE_NAME, major_number);

	tabletClass = class_create("wacom_tablet_class");
	if (IS_ERR(tabletClass)) {
		unregister_chrdev(major_number, DEVICE_NAME);
		printk(KERN_ALERT "Failed to register device class\n");
		return PTR_ERR(tabletClass);
	}

	cdev_init(&dev_data.cdev, &fops);
	dev_data.cdev.owner = THIS_MODULE;
	cdev_add(&dev_data.cdev, MKDEV(major_number, 0), 1);
	printk(KERN_INFO "Device node created at /dev/%s\n", DEVICE_NAME);

	tabletDevice = device_create(tabletClass, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
	if (IS_ERR(tabletDevice)) {
		class_destroy(tabletClass);
		unregister_chrdev(major_number, DEVICE_NAME);
		printk(KERN_ALERT "Failed to create the device\n");
		return PTR_ERR(tabletDevice);
	}

	result = register_input_device();
	if (result) {
		device_destroy(tabletClass, MKDEV(major_number, 0));
		class_destroy(tabletClass);
		unregister_chrdev(major_number, DEVICE_NAME);
		return result;
	}

	// Set up the URB for asynchronous interrupt transfers.
	wacom_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!wacom_urb) {
		printk(KERN_ALERT "Failed to allocate URB\n");
		return -ENOMEM;
	}
	wacom_buf = usb_alloc_coherent(usb_dev, URB_BUFFER_SIZE, GFP_KERNEL, &wacom_urb->transfer_dma);
	if (!wacom_buf) {
		usb_free_urb(wacom_urb);
		printk(KERN_ALERT "Failed to allocate transfer buffer\n");
		return -ENOMEM;
	}

	// Use the interrupt endpoint (0x81) as per your lsusb output.
	pipe = usb_rcvintpipe(usb_dev, 0x81);
	interval = 1; // bInterval from descriptor is 1

	usb_fill_int_urb(wacom_urb, usb_dev, pipe, wacom_buf, URB_BUFFER_SIZE,
	                 wacom_usb_read_callback, tablet_input_dev, interval);

	result = usb_submit_urb(wacom_urb, GFP_KERNEL);
	if (result) {
		usb_free_coherent(usb_dev, URB_BUFFER_SIZE, wacom_buf, wacom_urb->transfer_dma);
		usb_free_urb(wacom_urb);
		printk(KERN_ALERT "Failed to submit URB: error %d\n", result);
		return result;
	}
	printk(KERN_INFO "URB submitted successfully\n");
	printk(KERN_INFO "WacomDriver - Probe executed\n");
	return 0;
}

// DISCONNECT FUNCTION: Called when the device is unplugged.
static void wacom_usb_disconnect(struct usb_interface *intf)
{
	struct usb_device *usb_dev = interface_to_usbdev(intf);

	if (wacom_urb) {
		usb_kill_urb(wacom_urb);
		usb_free_coherent(usb_dev, URB_BUFFER_SIZE, wacom_buf, wacom_urb->transfer_dma);
		usb_free_urb(wacom_urb);
		wacom_urb = NULL;
		wacom_buf = NULL;
	}
	unregister_input_device();
	device_destroy(tabletClass, MKDEV(major_number, 0));
	class_destroy(tabletClass);
	unregister_chrdev(major_number, DEVICE_NAME);
	printk(KERN_INFO "WacomDriver - Disconnect executed\n");
}

static struct usb_driver my_usb_driver = {
	.name = "WacomDeviceDriver",
	.id_table = my_usb_table,
	.probe = wacom_usb_probe,
	.disconnect = wacom_usb_disconnect,
	.supports_autosuspend = 1,
};

static int __init wacom_init(void)
{
	int usb_result;
	init_proc();
	usb_result = usb_register(&my_usb_driver);
	if (usb_result) {
		printk(KERN_ALERT "USB driver registration failed.\n");
		return -EFAULT;
	}
	return 0;
}

static void __exit wacom_exit(void)
{
	exit_proc();
	usb_deregister(&my_usb_driver);
	printk(KERN_INFO "Device unregistered\n");
}

module_init(wacom_init);
module_exit(wacom_exit);

