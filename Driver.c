#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h> // for device registration
#include <linux/uaccess.h> // provides functions to copy data from user space
#include <linux/proc_fs.h> // for proc file
#include <linux/device.h>
#include <linux/input.h> // input device handling
#include <linux/cdev.h>
#include <linux/usb.h>

#define DEVICE_NAME "wacom-tablet" // name of device
#define BUFFER_SIZE 1024 // size of internal buffer

#define DEVICE_VENDOR_ID 0x056a
//temp for yasmin whilst working on the other tablet
#define DEVICE_PRODUCT_ID 0x0357
//#define DEVICE_PRODUCT_ID 0x033b

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yasmin, David, Waleed and April");
MODULE_DESCRIPTION("Wacom tablet device driver.");
MODULE_VERSION("1.0");

static int major_number; // stores allocated major number
static char buffer[BUFFER_SIZE]; // internal buffer size
static size_t buffer_data_size = 0; // keeps track of how much data is stored in the buffer

// Input device structure
static struct input_dev *tablet_input_dev = NULL;

static struct proc_dir_entry *pentry;

// Pointers for device creation
static struct class *tabletClass = NULL;
static struct device *tabletDevice = NULL;

//gives our device an inbuilt Linux character device structure
struct device_data {
	struct cdev cdev;
};

static struct device_data dev_data;

//FILE OPERATIONS-----------------------------------------------------------------------------------------

// Shows that device is opened in kernel
static int device_open(struct inode *inode, struct file *file) {
    printk(KERN_INFO "Device opened\n");
    return 0;
}

// Shows that device is released in kernel
static int device_release(struct inode *inode, struct file *file) {
    printk(KERN_INFO "Device released\n");
    return 0;
}

// Function to handle read operations
static ssize_t device_read(struct file *file, char __user *user_buffer, size_t len, loff_t *offset) {
    size_t bytes_to_read = min(len, buffer_data_size);
    if (copy_to_user(user_buffer, buffer, bytes_to_read)) {
        return -EFAULT;
    }

    printk(KERN_INFO "Device read %zu bytes\n", bytes_to_read);
    return bytes_to_read;
}

static struct file_operations fops = {
    .open = device_open,
    .release = device_release,
    .read = device_read,
};
//-----------------------------------------------------------------------------------------------------

//INPUT DEVICE STUFF---------------------------------------------------------------------
// Function to register the input device
static int register_input_device(void) {
    int error;

    // Allocate memory for the input device
    tablet_input_dev = input_allocate_device();
    if (!tablet_input_dev) {
        printk(KERN_ALERT "Failed to allocate input device\n");
        return -ENOMEM;
    }

    tablet_input_dev->name = "Wacom Tablet Input Device";
    tablet_input_dev->id.bustype = BUS_USB; // Set to USB bus type
    tablet_input_dev->evbit[0] = BIT(EV_ABS) | BIT(EV_KEY); // Handle ABS events (coordinates), key events (buttons)
    input_set_abs_params(tablet_input_dev, ABS_X, 0, 10000, 0, 0); // X axis
    input_set_abs_params(tablet_input_dev, ABS_Y, 0, 10000, 0, 0); // Y axis
    input_set_abs_params(tablet_input_dev, ABS_PRESSURE, 0, 255, 0, 0); // Pressure levels

    // Register the input device
    error = input_register_device(tablet_input_dev);
    if (error) {
        printk(KERN_ALERT "Failed to register input device\n");
        input_free_device(tablet_input_dev);
        return error;
    }

    printk(KERN_INFO "Input device registered\n");
    return 0;
}

// Function to unregister the input device
static void unregister_input_device(void) {
    if (tablet_input_dev) {
        input_unregister_device(tablet_input_dev);
        input_free_device(tablet_input_dev);
        printk(KERN_INFO "Input device unregistered\n");
    }
}
//---------------------------------------------------------------------------------------------------

//PROC STUFF-----------------------------------------------------------------------------------------
static struct proc_ops pops={
};

static int init_proc(void){
	pentry = proc_create(DEVICE_NAME, 0644, NULL, &pops);
	if(pentry == NULL){
		printk(KERN_ALERT "Failed to create proc entry");
		return -EFAULT;
	}
	printk(KERN_INFO "Proc file successfully created at /proc/%s", DEVICE_NAME);

	return 0;
}
//-------------------------------------------------------------------------------------------------------


//USB DEVICE STUFF-------------------------------------------------------------------------------------
//last argument always has to be empty
static struct usb_device_id my_usb_table[] = 
{
    {USB_DEVICE(DEVICE_VENDOR_ID, DEVICE_PRODUCT_ID)}, 
    {}, 
};
MODULE_DEVICE_TABLE(usb, my_usb_table); 

/**wacom_usb_probe() - initializes when device is plugged in
@intf: USB interface
@id: USB device id

Putting some initializations here means that we don't need to use up space if the device isn't plugged in.
*/
static int wacom_usb_probe(struct usb_interface *intf, const struct usb_device_id *id) { 	
    int chrdev_result;
	int result;
    dev_t dev;

	chrdev_result = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);

	major_number = MAJOR(dev);

	if(major_number < 0){
		printk(KERN_ALERT "Failed to register major number");
		return major_number;
	}

    printk(KERN_INFO "%s device registered with major number %d\n", DEVICE_NAME, major_number);
	
    // Create the device class
    tabletClass = class_create("wacom_tablet_class");
    if (IS_ERR(tabletClass)) {
        unregister_chrdev(major_number, DEVICE_NAME);
        printk(KERN_ALERT "Failed to register device class\n");
        return PTR_ERR(tabletClass);
    }


	//initializes character device and connects it with our major number
	cdev_init(&dev_data.cdev, &fops);
	dev_data.cdev.owner = THIS_MODULE;
	cdev_add(&dev_data.cdev, MKDEV(major_number, 0), 1);
	
    printk(KERN_INFO "Device node created at /dev/%s\n", DEVICE_NAME);

    // Automatically create the device node in /dev
    tabletDevice = device_create(tabletClass, NULL, MKDEV(major_number, 0), NULL, DEVICE_NAME);
    if (IS_ERR(tabletDevice)) {
        class_destroy(tabletClass);
        unregister_chrdev(major_number, DEVICE_NAME);
        printk(KERN_ALERT "Failed to create the device\n");
        return PTR_ERR(tabletDevice);
    }

    // Register the input device
    result = register_input_device();
    if (result) {
        device_destroy(tabletClass, MKDEV(major_number, 0));
        class_destroy(tabletClass);
        unregister_chrdev(major_number, DEVICE_NAME);
        return result;
    }

    printk("WacomDriver - The Probe function has executed\n");
    return 0; 
}


//this function is executed when the usb is ejected out 
static void wacom_usb_disconnect(struct usb_interface *intf) { 
      printk("WacomDriver - The Exit function has executed\n");
      
       
}
static struct usb_driver my_usb_driver =
{
    .name = "WacomDeviceDriver",
    .id_table = my_usb_table,
    .probe = wacom_usb_probe,
    .disconnect = wacom_usb_disconnect,
    .supports_autosuspend = 1
};
//---------------------------------------------------------------------------------------

/** wacom_init() - initializes device node, proc file, usb and character device for our driver.
*/
static int __init wacom_init(void) {
	int usb_result;

	//initializes proc file
    init_proc();
	
	usb_result = usb_register(&my_usb_driver);
	if (usb_result) {
    	printk(KERN_ALERT "USB driver registration failed.\n");
		return -EFAULT;
	}

    return 0;
}


static void exit_proc(void){
	proc_remove(pentry);
	printk(KERN_INFO "Proc file at /proc/%s removed.", DEVICE_NAME);
}
// Exit function
static void __exit wacom_exit(void) {
    unregister_input_device();
    exit_proc();

    device_destroy(tabletClass, MKDEV(major_number, 0));
    class_destroy(tabletClass);
    unregister_chrdev(major_number, DEVICE_NAME);

	usb_deregister(&my_usb_driver);


    printk(KERN_INFO "Device unregistered\n");
}

// Register module entry and exit points
module_init(wacom_init);
module_exit(wacom_exit);

