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
#include <linux/mutex.h>           // for mutexes
#include <linux/printk.h>
#include <linux/workqueue.h>       // for deferred work

#define DEVICE_NAME "ISE_mouse"
#define BUFFER_SIZE 1024
#define URB_BUFFER_SIZE 64

#define DEVICE_VENDOR_ID 0x046d
#define DEVICE_PRODUCT_ID 0xc063

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yasmin, David, Waleed, April, merged with deferred logging by Your Name");
MODULE_DESCRIPTION("Mouse Driver");
MODULE_VERSION("1.0");

/* Global variables */
static int major_number;
static char buffer[BUFFER_SIZE];
static size_t buffer_data_size = 0;
static struct proc_dir_entry *pentry = NULL;
static struct class *tabletClass = NULL;
static struct device *tabletDevice = NULL;
struct device_data {
	struct cdev cdev;
};
static struct device_data dev_data;

/* Use a mutex (instead of a spinlock) for protecting the log buffer in process context */
static DEFINE_MUTEX(buffer_mutex);
/* Character device file operations */
static int device_open(struct inode *inode, struct file *file)
{
	printk(KERN_INFO "Mouse device opened\n");
	return 0;
}

static int device_release(struct inode *inode, struct file *file)
{
	printk(KERN_INFO "Mouse device released\n");
	return 0;
}

static ssize_t device_read(struct file *file, char __user *user_buffer,
                           size_t len, loff_t *offset)
{
	size_t bytes_to_read;
	int ret;

	/* Use a mutex here since we're in process context */
	mutex_lock(&buffer_mutex);
	bytes_to_read = min(len, buffer_data_size);
	if (bytes_to_read == 0) {
		mutex_unlock(&buffer_mutex);
		return 0;
	}
	ret = copy_to_user(user_buffer, buffer, bytes_to_read);
	if (ret) {
		mutex_unlock(&buffer_mutex);
		return -EFAULT;
	}
	/* Remove the data that was read */
	memmove(buffer, buffer + bytes_to_read, buffer_data_size - bytes_to_read);
	buffer_data_size -= bytes_to_read;
	mutex_unlock(&buffer_mutex);

	printk(KERN_INFO "Mouse device read %zu bytes\n", bytes_to_read);
	return bytes_to_read;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = device_open,
	.release = device_release,
	.read = device_read,
};

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

/* USB device table */
static struct usb_device_id my_usb_table[] = {
	{ USB_DEVICE(DEVICE_VENDOR_ID, DEVICE_PRODUCT_ID) },
	{ },
};
MODULE_DEVICE_TABLE(usb, my_usb_table);

/* Probe function: sets up the character device, registers the input device, and submits the URB */
static int wacom_usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	int chrdev_result, result;
	dev_t dev;

	chrdev_result = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
	major_number = MAJOR(dev);
	if (major_number < 0) {
		printk(KERN_ALERT "Failed to register major number\n");
		return major_number;
	}
	printk(KERN_INFO "%s device registered with major number %d\n", DEVICE_NAME, major_number);

	tabletClass = class_create("mouse_class");
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

	printk(KERN_INFO "URB submitted successfully\n");
	printk(KERN_INFO "Mouse driver - Probe executed AAAAAAAAAA\n");
	return 0;
}

/* Disconnect function: cleans up URBs, input device, and character device */
static void wacom_usb_disconnect(struct usb_interface *intf)
{

	device_destroy(tabletClass, MKDEV(major_number, 0));
	class_destroy(tabletClass);
	unregister_chrdev(major_number, DEVICE_NAME);
	printk(KERN_INFO "Mouse - Disconnect executed\n");
}

static struct usb_driver my_usb_driver = {
	.name = "MouseDriver",
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
	printk(KERN_INFO "Mouse device unregistered\n");
}

module_init(wacom_init);
module_exit(wacom_exit);
