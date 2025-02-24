#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h> //for device registration
#include <linux/uaccess.h> //provides functions to copy data from user space
#include <linux/proc_fs.h> //for proc file

#define DEVICE_NAME "loopback" //name of device
#define BUFFER_SIZE 1024 //size of internal buffer

//vendor and product ID of wacom tablet gotten from lsusb
#define DEVICE_VENDOR_ID = 0x56a
#define DEVICE_PRODUCT_ID = 0x033b

//proc file system name
#define proc_name = "wacom-device-tablet"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yasmin");
MODULE_DESCRIPTION("Wacom tablet device driver.");
MODULE_VERSION("1.0");

static int major_number; //stores dynamic allocated major number.
static char buffer [BUFFER_SIZE]; //internal buffer size
static size_t buffer_data_size = 0; //keeps track of how much data is stored in the buffer

static proc_entry* proc_file; //pointer that will to be /proc file

/*
static int proc_read(){
	return 0;
}

*/

static const struct proc_ops proc_fops = {
	.owner = THIS_MODULE,
	.open = proc_open,
	.read = seq_read,
};

static struct file_operations fops={
	.open = device_open,
	.release = device_release,
	.read = device_read,
	.write = device_write,
};

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
static ssize_t device_read(struct file *file, const char __user *user_buffer, size_t len, loff_t *offset){
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
static ssize_t device_write(struct file *file, const char __user *user_buffer, size_t len, loff_t *offset){
	size_t bytes_to_write = min(len, (size_t)(BUFFER_SIZE -1));

	if(copy_from_user(bugger, user_buffer, bytes_to_write)){
		return -EFAULT;
	}
	buffer[bytes_to_write] = '\0'; //terminates program if nothing to write
	buffer_data_size = bytes_to_write;

	printk(KERN_INFO "Device wrote %zu bytes.\n", bytes_to_write);

	return bytes_to_write;
}

// Function called when the module is loaded
static int __init my_module_init(void) {
	init_proc_file();
    printk(KERN_INFO "Hello, Kernel! Module loaded.\n");
    return 0; // Return 0 means success
}

//initialize proc file
static int init_proc_file(){
	//creates proc file with reading access. Owner can write as well
	 proc_file = proc_create(proc_name, 0644, NULL, proc_fops);
	 if(proc_file == NULL){
	 	return -ENOMEM;
	 }
	 printk(KERN_INFO "Proc file /proc/%s successfully created.", proc_name);
	 return 0;
}

// Function called when the module is unloaded
static void __exit my_module_exit(void) {
	remove_proc_entry(proc_name, NULL);
	printk(KERN_INFO "Proc file /proc/%s successfully removed.", proc_name);
         // Hello world
    printk(KERN_INFO "Goodbye, Kernel! Module unloaded.\n");
}

/*
static int __init loopback_init(void){
	major_number = register_chrdev(0, DEVICE_NAME, &fops);
	if(major_number < 0){
		printk(KERN_ALERT "Failed to register major number\n");
		return major_number;
	}
	printk(KERN_INFO "Loopback device registered with major numebr %d\n", major_number);

	return 0;
}

static void __exit loopback_exit(void){
	unregister_chrdev(major_number, DEVICE_NAME);
	printk(KERN_INFO "Loopback device unregistered\n");
}
*/

// Register module entry and exit points
module_init(loopback_init);
module_exit(loopback_exit);
