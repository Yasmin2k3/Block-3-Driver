#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>              // for device registration
#include <linux/uaccess.h>         // copy_to_user
#include <linux/proc_fs.h>         // proc file
#include <linux/device.h>
#include <linux/input.h>           // input device handling
#include <linux/cdev.h>
#include <linux/usb.h>        // for mutexes
#include <linux/printk.h>      // for deferred work
#include <linux/hid.h> //for usbhid

#define DEVICE_NAME "ISE_mouse_driver"
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
static struct class *mouse_class = NULL;
static struct device *mouse_device = NULL;
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

/* HID device table. This should be more specific than the usbhid driver. (please)*/
static struct hid_device_id mouse_hid_table[] = {
	{ HID_USB_DEVICE(DEVICE_VENDOR_ID, DEVICE_PRODUCT_ID) },
	{ },
};
MODULE_DEVICE_TABLE(hid, mouse_hid_table);

/*
* mouse_usb_probe Initializes  USB driver.
*
* This is only called when it detects a usb with our vendor and product ID.
*/
static int mouse_usb_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int chrdev_result;
	dev_t dev;

 //Dynamically allocates a region in memory for our character device.
	chrdev_result = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
	major_number = MAJOR(dev);
	//Error handling
	if (major_number < 0) {
		printk(KERN_ALERT "Failed to register major number\n");
		return major_number;
	}
	printk(KERN_INFO "%s device registered with major number %d\n", DEVICE_NAME, major_number);

  //Creates a 
	mouse_class = class_create("mouse_class");
	if (IS_ERR(mouse_class)) {
		unregister_chrdev(major_number, DEVICE_NAME);
		printk(KERN_ALERT "Failed to register device class\n");
		return PTR_ERR(mouse_class);
	}

	cdev_init(&dev_data.cdev, &fops);
	dev_data.cdev.owner = THIS_MODULE;
	cdev_add(&dev_data.cdev, MKDEV(major_number, 0), 1);
	printk(KERN_INFO "Device node created at /dev/%s\n", DEVICE_NAME);

	mouse_device = device_create(mouse_class, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
	if (IS_ERR(mouse_device)) {
		class_destroy(mouse_class);
		unregister_chrdev(major_number, DEVICE_NAME);
		printk(KERN_ALERT "Failed to create the device\n");
		return PTR_ERR(mouse_device);
	}

	init_proc();

	printk(KERN_INFO "URB submitted successfully\n");
	printk(KERN_INFO "Mouse driver - Probe executed PPLEEEEEAAAAAAAAAASE\n");
	return 0;
}

/* 
* mouse_usb_remove Cleans up usb device
*
* This is called when the usb is removed.
*/
static void mouse_usb_remove(struct hid_device *hdev)
{
	exit_proc();
	device_destroy(mouse_class, MKDEV(major_number, 0));
	class_destroy(mouse_class);
	unregister_chrdev(major_number, DEVICE_NAME);
	printk(KERN_INFO "Mouse - Disconnect executed\n");
}

static struct hid_driver mouse_hid_driver = {
	.name = "mouse_driver",
	.id_table = mouse_hid_table,
	.probe = mouse_usb_probe,
	.remove = mouse_usb_remove,
};

static int __init mouse_init(void)
{
	int hid_result;

	hid_result = hid_register_driver(&mouse_hid_driver);
	if (hid_result) {
		printk(KERN_ALERT "USB driver registration failed.\n");
		return -EFAULT;
	}
	return 0;
}

/*
* mouse_exit - Unregisters driver :)
*/
static void __exit mouse_exit(void)
{

	hid_unregister_driver(&mouse_hid_driver);
	printk(KERN_INFO "Mouse device unregistered\n");
}

module_init(mouse_init);
module_exit(mouse_exit);
