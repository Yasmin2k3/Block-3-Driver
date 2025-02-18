#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h> //for device registration
#include <linux/uaccess.h> //provides functions to copy data from user space

#define DEVICE_NAME "loopback" //name of device
#define BUFFER_SIZE 1024 //size of internal buffer

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yasmin");
MODULE_DESCRIPTION("A simple kernel module example");
MODULE_VERSION("1.0");

static int major_number; //stores dynamic allocated major number.
static char buffer [BUFFER_SIZE]; //internal buffer size
static size_t buffer_data_size = 0; //keeps track of how much data is stored in the buffer

//Shows that device is opened in kernel
static int device_open(struct inode *inode, struct file *file) {
	printk(KERN_INFO "Device opened\n");
	return 0;
}

//Shows that device is released in kernel
static int device_release(struct inode *inode, struct file *file) {
	printk(KERN_INFO "Device released\n");
	return 0;
}

//function to handle read operations
static ssize_t device_read(strict file *file, const char __user *user_buffer, size_t len, loff_t *offset){
	//determine minimum of requested length and available data
	size_t bytes_to_read = min(len, buffer_data_size);
	//copy data in to user space
	if (copy_to_user(user_buffer, buffer, bytes_to_read)) {
		//if buffer is too big to copy to user
		return -EFAULT;
	}
	//refresh data in buffer
	buffer_data_size = 0;
	//log device upon read
	printk(KERN_INFO "Device read %zu bytes\n" bytes_to_read);
	return bytes_to_read;
}

//function to handle write operations

// Function called when the module is loaded
static int __init my_module_init(void) {
    printk(KERN_INFO "Hello, Kernel! Module loaded.\n");
    return 0; // Return 0 means success
}

// Function called when the module is unloaded
static void __exit my_module_exit(void) {
    printk(KERN_INFO "Goodbye, Kernel! Module unloaded.\n");
}

// Register module entry and exit points
module_init(my_module_init);
module_exit(my_module_exit);
