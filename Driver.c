#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>              // for device registration
#include <linux/uaccess.h>         // copy_to_user
#include <linux/proc_fs.h>         // proc file
#include <linux/device.h>
#include <linux/input.h>           // input device handling
#include <linux/cdev.h>
#include <linux/usb.h>            // for mutexes
#include <linux/printk.h>         // for deferred work
#include <linux/hid.h>            // for usbhid

#define DEVICE_NAME "ISE_mouse_driver"
#define BUFFER_SIZE 1024
#define URB_BUFFER_SIZE 64

#define DEVICE_VENDOR_ID 0x046d
#define DEVICE_PRODUCT_ID 0xc063

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yasmin, David, Waleed, April");
MODULE_DESCRIPTION("Mouse Driver");
MODULE_VERSION("1.0");

// Global variables 
static int major_number;
static char buffer[BUFFER_SIZE];
static size_t buffer_data_size = 0;
static struct proc_dir_entry *pentry = NULL;
static struct class *mouse_class = NULL;
static struct device *mouse_device = NULL;
static struct input_dev *mouse_input;
struct device_data {
    struct cdev cdev;
};
static struct device_data dev_data;

long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

//values to be tracked for the proc file
static int left_mouse_clicked=0;
static int right_mouse_clicked=0;


// Mutex for protecting the log buffer.
static DEFINE_MUTEX(buffer_mutex);

/* Button status tracking (for ioctl)
* 0: None, 1: Left, 2: Right, 3: Middle
*/
static int button_status = 0; 

// IOCTL commands. M: unique identifier for 'magic number' device type to differentiate IOCTL.
//Reads current button status from driver
#define IOCTL_GET_BUTTON_STATUS _IOR('M', 1, int)
//Writes button status to the driver
#define IOCTL_SET_BUTTON_STATUS _IOW('M', 2, int)

/*
* device_open logs when /dev/ISE_mouse_drive is open for reading
*/
static int device_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "Mouse device opened\n");
    return 0;
}

/*
* device_release logs when /dev/ISE_mouse_driver has been read and is closed.
*/
static int device_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "Mouse device released\n");
    return 0;
}

/*
* device_read reads from /dev/ISE_mouse_driver into user space.
*/
static ssize_t device_read(struct file *file, char __user *user_buffer,
                           size_t len, loff_t *offset)
{
    size_t bytes_to_read;
    int ret;

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

/*
* device_ioctl copies and/or updates button_status.
*
* copies button_status to user space when IOCTL_GET_BUTTON_STATUS is called
* updates user input when IOCTL_SET_BUTTON_STATUS is called
*
* returns -EINVAL if unknown command is sent.
*/
long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret = 0;
    switch (cmd) {
        case IOCTL_GET_BUTTON_STATUS:
            // Copy button status to user space
            if (copy_to_user((int *)arg, &button_status, sizeof(button_status))) {
                ret = -EFAULT;
            }
            break;

        case IOCTL_SET_BUTTON_STATUS:
            // Set button status from user space
            if (copy_from_user(&button_status, (int *)arg, sizeof(button_status))) {
                ret = -EFAULT;
            }
            break;

        default:
            ret = -EINVAL; // Invalid command
            break;
    }
    return ret;
}

//Registers file operations for character device.
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_release,
    .read = device_read,
    .unlocked_ioctl = device_ioctl,
};


/*
* read_proc reads from proc file into user space
*/
static ssize_t read_proc(struct file *file, char __user *user_buf, size_t count, loff_t *pos) {
    char buf[128];
    int len = snprintf(buf, sizeof(buf), DEVICE_NAME "\nLeft Mouse Clicked: %d\n Right Mouse Clicked: %d\n", left_mouse_clicked, right_mouse_clicked);
    
    return simple_read_from_buffer(user_buf, count, pos, buf, len);
}

/*
* write_proc writes to proc file
* 
* Stores how many times the left and right mouse buttons are clicked respectively.
*/
static ssize_t write_proc(struct file *file, const char __user *user_buf, size_t count, loff_t *pos) {
    char buf[32];
    if (count > sizeof(buf) - 1)
        return -EINVAL;

    if (copy_from_user(buf, user_buf, count))
        return -EFAULT;
    //temp values to store
    int new_lclick=0;
    int new_rclick=0;
    buf[count] = '\0';
    //scans for two integers. Throws an error if less than two appear for whatever reason
    if (sscanf(buf,"%d %d",&new_lclick,&new_rclick)!=2) // Convert user input to int
        return -EINVAL;
    left_mouse_clicked=new_lclick;
    right_mouse_clicked=new_rclick;
    return count;
}

//Registers operations for proc file.
static struct proc_ops pops = { 
    .proc_read = read_proc,
    .proc_write = write_proc,
};

/*
* init_proc creates proc file in /proc file directory.
*
* returns -EFAULT if failed.
*/
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

/*
* removes proc file in /proc file directory
*/
static void exit_proc(void)
{
    proc_remove(pentry);
    printk(KERN_INFO "Proc file /proc/%s removed\n", DEVICE_NAME);
}

/* HID device table. */
static struct hid_device_id mouse_hid_table[] = {
    { HID_USB_DEVICE(DEVICE_VENDOR_ID, DEVICE_PRODUCT_ID) },
    { },
};
MODULE_DEVICE_TABLE(hid, mouse_hid_table);

/*
* mouse_input_init initializes input device for the mouse driver.
* 
* This could go in the probe but I've abstracted it for ease of reading.
*/
static int mouse_input_init(struct hid_device *hdev, const struct hid_device_id *id)
{
    int ret;

    ret = hid_parse(hdev);
    if (ret) {
        printk(KERN_ERR "HID parse failed: %d\n", ret);
        return ret;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
    if (ret) {
        printk(KERN_ERR "HID hw start failed: %d\n", ret);
        return ret;
    }

    mouse_input = input_allocate_device();
    if (!mouse_input) {
        printk(KERN_ERR "Failed to allocate input device\n");
        return -ENOMEM;
    }

    mouse_input->name = "ISE-mouse";
    mouse_input->phys = "ISE-mouse0";
    mouse_input->id.bustype = BUS_USB;
    mouse_input->id.vendor = id->vendor;
    mouse_input->id.product = id->product;
    mouse_input->id.version = 0x0100;

    set_bit(EV_REL, mouse_input->evbit);
    set_bit(REL_X, mouse_input->relbit);
    set_bit(REL_Y, mouse_input->relbit);
    set_bit(EV_KEY, mouse_input->evbit);
    set_bit(BTN_LEFT, mouse_input->keybit);
    set_bit(BTN_RIGHT, mouse_input->keybit);
    set_bit(BTN_MIDDLE, mouse_input->keybit);

    ret = input_register_device(mouse_input);
    if (ret) {
        input_free_device(mouse_input);
        printk(KERN_ERR "Failed to register input device\n");
        return ret;
    }

    return 0;
}

/*
* mouse_usb_probe initializes mouse device when the usb with matching product and vendor id is found.
*/
static int mouse_usb_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
    int chrdev_result;
    dev_t dev;

    mouse_input_init(hdev, id);

    chrdev_result = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
    major_number = MAJOR(dev);
    if (major_number < 0) {
        printk(KERN_ALERT "Failed to register major number\n");
        return major_number;
    }
    printk(KERN_INFO "%s device registered with major number %d\n", DEVICE_NAME, major_number);

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

    printk(KERN_INFO "Mouse driver - Probe executed\n");
    return 0;
}

/*
* mouse_usb_remove cleans up everything when usb is no longer connected.
*/
static void mouse_usb_remove(struct hid_device *hdev)
{
    hid_hw_stop(hdev);
    input_unregister_device(mouse_input);
    exit_proc();
    device_destroy(mouse_class, MKDEV(major_number, 0));
    class_destroy(mouse_class);
    unregister_chrdev(major_number, DEVICE_NAME);
    printk(KERN_INFO "Mouse - Disconnect executed\n");
}

/*
* mouse_raw_event logs raw event input into /dev/ISE_mouse_driver
*/
static int mouse_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
    int buttons;
    int x_delta;
    int y_delta;

    if (size < 3) {
        return 0;
    }

    buttons = data[0];
    x_delta = (int)((signed char)data[1]);
    y_delta = (int)((signed char)data[2]);

    input_report_rel(mouse_input, REL_X, x_delta);
    input_report_rel(mouse_input, REL_Y, y_delta);
    input_sync(mouse_input);

    unsigned int available = BUFFER_SIZE - buffer_data_size;

    if (buttons & (1 << 0)) {
        printk(KERN_INFO "Left Button Pressed\n");
        snprintf(buffer + buffer_data_size, available, "Left Button Pressed");
        button_status = 1; // Left button pressed
    }

    if (buttons & (1 << 1)) {
        printk(KERN_INFO "Right Button Pressed\n");
        button_status = 2; // Right button pressed
    }

    if (buttons & (1 << 2)) {
        printk(KERN_INFO "Middle button Pressed\n");
        button_status = 3; // Middle button pressed
    }

    return 0;
}

//Initializes hid driver for mouse. Used HID rather than USB because this is more specific, and therefore USBHID doesnt automatically take control.
static struct hid_driver mouse_hid_driver = {
    .name = "mouse_driver",
    .id_table = mouse_hid_table,
    .probe = mouse_usb_probe,
    .remove = mouse_usb_remove,
    .raw_event = mouse_raw_event,
};


/*
* mouse_init registers our driver as a hid driver.
* 
* called with insmod.
*/
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
* mouse_exit cleans up our driver.
*
* called with rmmod.
*/
static void __exit mouse_exit(void)
{
    hid_unregister_driver(&mouse_hid_driver);
    printk(KERN_INFO "Mouse device unregistered\n");
}

module_init(mouse_init);
module_exit(mouse_exit);
