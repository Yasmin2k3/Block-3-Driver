#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>              // for device registration
#include <linux/uaccess.h>         // copy_to_user
#include <linux/proc_fs.h>         // proc file
#include <linux/device.h>
#include <linux/input.h>           // input device handling
#include <linux/cdev.h>
#include <linux/usb.h>
#include <linux/spinlock.h>
#include <linux/printk.h>

#define DEVICE_NAME "wacom-tablet"
#define BUFFER_SIZE 1024
#define URB_BUFFER_SIZE 64

#define DEVICE_VENDOR_ID 0x056a
#define DEVICE_PRODUCT_ID 0x0357

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yasmin, David, Waleed and April");
MODULE_DESCRIPTION("Wacom tablet driver with URB-based asynchronous reads and numbered buttons.");
MODULE_VERSION("1.0");

/* Global variables */
static int major_number;
static char buffer[BUFFER_SIZE];
static size_t buffer_data_size = 0;
static struct input_dev *tablet_input_dev = NULL;
static struct proc_dir_entry *pentry = NULL;
static struct class *tabletClass = NULL;
static struct device *tabletDevice = NULL;
struct device_data {
	struct cdev cdev;
};
static struct device_data dev_data;
static struct urb *wacom_urb = NULL;
static unsigned char *wacom_buf = NULL;

/* Spinlock to protect access to the internal buffer */
static DEFINE_SPINLOCK(buffer_lock);


//following was gotten from the wacom documentation------------------------------------
/* Helper functions to map numbered buttons to key codes */
static int wacom_numbered_button_to_key(int n)
{
	if (n < 10)
		return BTN_0 + n;
	else if (n < 16)
		return BTN_A + (n - 10);
	else if (n < 18)
		return BTN_BASE + (n - 16);
	else
		return 0;
}

static void wacom_setup_numbered_buttons(struct input_dev *input_dev, int button_count)
{
	int i;
	for (i = 0; i < button_count; i++) {
		int key = wacom_numbered_button_to_key(i);
		if (key)
			__set_bit(key, input_dev->keybit);
	}
}
//----------------------------------------------------------------------------------------

/* File operations for the character device */
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
	size_t bytes_to_read;
	int ret;
	//spinlock not good, chenge it to something else later.
	spin_lock(&buffer_lock);
	bytes_to_read = min(len, buffer_data_size);
	if (bytes_to_read == 0) {
		spin_unlock(&buffer_lock);
		return 0;
	}
	ret = copy_to_user(user_buffer, buffer, bytes_to_read);
	if (ret) {
		spin_unlock(&buffer_lock);
		return -EFAULT;
	}
	memmove(buffer, buffer + bytes_to_read, buffer_data_size - bytes_to_read);
	buffer_data_size -= bytes_to_read;
	spin_unlock(&buffer_lock);

	printk(KERN_INFO "Device read %zu bytes\n", bytes_to_read);
	return bytes_to_read;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = device_open,
	.release = device_release,
	.read = device_read,
};

/* Input device registration */
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
	/* Enable absolute events for coordinates and pressure */
	tablet_input_dev->evbit[0] = BIT(EV_ABS) | BIT(EV_KEY);
	input_set_abs_params(tablet_input_dev, ABS_X, 0, 10000, 0, 0);
	input_set_abs_params(tablet_input_dev, ABS_Y, 0, 10000, 0, 0);
	input_set_abs_params(tablet_input_dev, ABS_PRESSURE, 0, 255, 0, 0);

	/* Set up numbered buttons (assume 18 buttons) */
	wacom_setup_numbered_buttons(tablet_input_dev, 18);

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

/* Proc file setup */
static struct proc_ops pops = { };

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

//I'll actually figure out what this shit does tomorrow and try refine it

/* URB callback to process incoming reports */
static void wacom_usb_read_callback(struct urb *urb)
{
	int status = urb->status;
	uint32_t btn_mask;
	int i;

	if (status) {
		printk(KERN_INFO "URB read error: %d\n", status);
		goto resubmit;
	}

	/* Expect at least 4 bytes for a 32-bit bitmask of buttons */
	if (urb->actual_length >= 4) {
		btn_mask = wacom_buf[0] |
		           (wacom_buf[1] << 8) |
		           (wacom_buf[2] << 16) |
		           (wacom_buf[3] << 24);

		/* Loop over each button up to 18 buttons */
		for (i = 0; i < 18; i++) {
			int key = wacom_numbered_button_to_key(i);
			int state = (btn_mask & (1 << i)) ? 1 : 0;
			/* Invert state if testing shows pressed/released reversed */
			state = !state;
			input_event(tablet_input_dev, EV_KEY, key, state);
		}
		input_sync(tablet_input_dev);

		//change later methinks
		/* Optionally, log the button mask to the internal buffer */
		spin_lock(&buffer_lock);
		{
			unsigned int avail = BUFFER_SIZE - buffer_data_size;
			if (avail > 0) {
				int ret = snprintf(buffer + buffer_data_size, avail,
				                   "Reading: 0x%08x\n", btn_mask);
				if (ret > 0) {
					buffer_data_size += ret;
					if (buffer_data_size >= BUFFER_SIZE)
						buffer_data_size = BUFFER_SIZE;
				}
			}
		}
		spin_unlock(&buffer_lock);
	}

resubmit:
	usb_submit_urb(urb, GFP_ATOMIC);
}

/* USB device table */
static struct usb_device_id my_usb_table[] = {
	{ USB_DEVICE(DEVICE_VENDOR_ID, DEVICE_PRODUCT_ID) },
	{ },
};
MODULE_DEVICE_TABLE(usb, my_usb_table);

/* Probe function */
static int wacom_usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
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

	/* Allocate and submit the URB for asynchronous interrupt transfers */
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

	/* Use interrupt endpoint 0x81 (as seen in lsusb output) */
	pipe = usb_rcvintpipe(usb_dev, 0x81);
	interval = 1;  // bInterval from the descriptor

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

/* Disconnect function */
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

