#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>              // device registration
#include <linux/uaccess.h>         // copy_to_user
#include <linux/proc_fs.h>         // proc file
#include <linux/device.h>
#include <linux/input.h>           // input device handling
#include <linux/cdev.h>
#include <linux/usb.h>             // for mutexes
#include <linux/hid.h>             // HID support
#include <linux/workqueue.h>
#include <linux/slab.h>					   // for kmalloc and kfree
#include <linux/smp.h>						 // to verify multithreading works

#define DEVICE_NAME "ISE_mouse_driver"
#define BUFFER_SIZE 1024

#define DEVICE_VENDOR_ID 0x046d
#define DEVICE_PRODUCT_ID 0xc063

// IOCTL commands.
#define IOCTL_GET_BUTTON_STATUS _IOR('M', 1, int)
#define IOCTL_SET_BUTTON_STATUS _IOW('M', 2, int)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yasmin, David, Waleed, April");
MODULE_DESCRIPTION("Mouse Driver");
MODULE_VERSION("1.0");

// Global variables 
static int major_number;
static char buffer[BUFFER_SIZE];
static size_t buffer_data_size;
static struct proc_dir_entry *pentry = NULL;
static struct class *mouse_class = NULL;
static struct device *mouse_device = NULL;
static struct input_dev *mouse_input;
struct device_data {
    struct cdev cdev;
};
static struct device_data dev_data;

long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

// Values tracked for the proc file
static int left_mouse_clicked;
static int right_mouse_clicked;

// Mutex for protecting the log buffer.
static DEFINE_MUTEX(buffer_mutex);

/* Button status tracking (for ioctl)
 * 0: None, 1: Left, 2: Right, 3: Middle
 */
static int button_status; 

// Workqueue for processing mouse events asynchronously
static struct workqueue_struct *mouse_wq;

// Structure for each mouse event work item
struct mouse_event {
    char message[128];
    struct work_struct work;
};

/*
 * device_open logs when /dev/ISE_mouse_driver is opened.
 */
static int device_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "Mouse device opened\n");
    return 0;
}

//IOCTL ---------------------------------------------------------------------------------------------------------------------------

/*
 * device_ioctl copies and/or updates button_status.
 */
long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    switch (cmd) {
        case IOCTL_GET_BUTTON_STATUS:
            if (copy_to_user((int *)arg, &button_status, sizeof(button_status)))
                return -EFAULT;
            break;
        case IOCTL_SET_BUTTON_STATUS:
            if (copy_from_user(&button_status, (int *)arg, sizeof(button_status)))
                return -EFAULT;
            break;
        default:
            return -EINVAL;
            break;
    }
    return 0;
}
//---------------------------------------------------------------------------------------------------------------------------------

//FILE OPERATION FUNCTIONS --------------------------------------------------------------------------------------------------------
/*
 * device_release logs when /dev/ISE_mouse_driver is closed.
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
		// Shift remaining data to the beginning of the buffer to prevent bad address error
    memmove(buffer, buffer + bytes_to_read, buffer_data_size - bytes_to_read);
    // Remove the data that was read
    buffer_data_size -= bytes_to_read;
    mutex_unlock(&buffer_mutex);

    printk(KERN_INFO "Mouse device read %zu bytes\n", bytes_to_read);
    return bytes_to_read;
}

// File operations for the character device
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = device_open,
    .release = device_release,
    .read = device_read,
    .unlocked_ioctl = device_ioctl,
};
//-----------------------------------------------------------------------------------------------------------------------------------------------

//PROC FILE FUNCTIONS ---------------------------------------------------------------------------------------------------------------------------

/*
 * read_proc reads from proc file into user space.
 */
static ssize_t read_proc(struct file *file, char __user *user_buf, size_t count, loff_t *pos)
{
    char buf[128];

		mutex_lock(&buffer_mutex);
    int len = snprintf(buf, sizeof(buf), "%s\nLeft Mouse Clicked: %d\nRight Mouse Clicked: %d\n", DEVICE_NAME, left_mouse_clicked, right_mouse_clicked);
		mutex_unlock(&buffer_mutex);

    return simple_read_from_buffer(user_buf, count, pos, buf, len);
}

/*
 * write_proc writes to proc file.
 *
 * only called when a userspace process writes to it  (i.e. echo).
 */
static ssize_t write_proc(struct file *file, const char __user *user_buf, size_t count, loff_t *pos)
{
    char buf[32];
    if (count > sizeof(buf) - 1)
        return -EINVAL;
    if (copy_from_user(buf, user_buf, count))
        return -EFAULT;
    
    int new_lclick = 0, new_rclick = 0;
    buf[count] = '\0';

		//lock and unlock before modifying variables    
    mutex_lock(&buffer_mutex);
    if (sscanf(buf, "%d %d", &new_lclick, &new_rclick) != 2){
				mutex_unlock(&buffer_mutex);
        return -EINVAL;
		}

    left_mouse_clicked = new_lclick;
    right_mouse_clicked = new_rclick;
    mutex_unlock(&buffer_mutex);

		return count;
}

// Proc file operations.
static struct proc_ops pops = { 
    .proc_read = read_proc,
    .proc_write = write_proc,
};

/*
 * init_proc creates a proc file in /proc.
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
 * exit_proc removes the proc file.
 */
static void exit_proc(void)
{
    proc_remove(pentry);
    printk(KERN_INFO "Proc file /proc/%s removed\n", DEVICE_NAME);
}

//-----------------------------------------------------------------------------------------------------------------------------------------------------------------

//HID DEVICE FUNCTIONS ----------------------------------------------------------------------------------------------------------------------------------------------

// HID device table.
static struct hid_device_id mouse_hid_table[] = {
    { HID_USB_DEVICE(DEVICE_VENDOR_ID, DEVICE_PRODUCT_ID) },
    { },
};
MODULE_DEVICE_TABLE(hid, mouse_hid_table);

/*
 * mouse_input_init initialises the input device.
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
        printk(KERN_ERR "HID start failed: %d\n", ret);
        return ret;
    }

    mouse_input = input_allocate_device();
    if (!mouse_input) {
        printk(KERN_ERR "Failed to allocate input device\n");
        return -ENOMEM;
    }

		//initializing mouse input device structure
    mouse_input->name = "ISE-mouse";
    mouse_input->phys = "ISE-mouse0";
    mouse_input->id.bustype = BUS_USB;
    mouse_input->id.vendor = id->vendor;
    mouse_input->id.product = id->product;
    mouse_input->id.version = 0x0100;

		//enables reporting of movement events and button clicks
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
 * mouse_usb_probe is called when a matching USB device is found.
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
 * mouse_usb_remove cleans up when the USB device is disconnected.
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

//RAW EVENT AND MULTITHREADING----------------------------------------------------------------------------------------------------------------------------------------
/*
 * mouse_event_worker runs in thread context.
 *
 * Appends the event's message to the global buffer.
 * Debug statements are added to log the current thread and CPU.
 */
static void mouse_event_worker(struct work_struct *work)
{
    struct mouse_event *event = container_of(work, struct mouse_event, work);
    size_t required_space = strlen(event->message);

    /* Debug: Log current thread and CPU information */
    printk(KERN_INFO "mouse_event_worker: PID=%d, CPU=%d processing event: %s",
           current->pid, smp_processor_id(), event->message);

    mutex_lock(&buffer_mutex);
    if ((BUFFER_SIZE - buffer_data_size) < required_space) {
        printk(KERN_INFO "mouse_event_worker: Buffer full, flushing buffer.\n");
        buffer_data_size = 0;
    }
    memcpy(buffer + buffer_data_size, event->message, required_space);
    buffer_data_size += required_space;
    mutex_unlock(&buffer_mutex);

    kfree(event);
}

/*
 * mouse_raw_event called on each raw HID event.
 *
 * Schedules work items for logging both mouse movement and button presses.
 */
static int mouse_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
    int buttons, x_delta, y_delta;
		struct mouse_event *event;

    if (size < 3)
        return 0;

    buttons = data[0];
    x_delta = (int)((signed char)data[1]);
    y_delta = (int)((signed char)data[2]);

    input_report_rel(mouse_input, REL_X, x_delta);
    input_report_rel(mouse_input, REL_Y, y_delta);
    input_sync(mouse_input);

		if (x_delta != 0 || y_delta != 0) {
        event = kmalloc(sizeof(*event), GFP_ATOMIC);
        if (event) {
            snprintf(event->message, sizeof(event->message),
                     "Mouse moved: X=%d, Y=%d\n", x_delta, y_delta);
            INIT_WORK(&event->work, mouse_event_worker);
            queue_work(mouse_wq, &event->work);
            printk(KERN_INFO "mouse_raw_event: Queued mouse move event.\n");
        }
    }

    if (buttons & (1 << 0)) {
        event = kmalloc(sizeof(*event), GFP_ATOMIC);
        if (event) {
            snprintf(event->message, sizeof(event->message),
                     "Left Button Pressed\n");
            INIT_WORK(&event->work, mouse_event_worker);
            queue_work(mouse_wq, &event->work);
            printk(KERN_INFO "mouse_raw_event: Queued left button event.\n");
        }
    }

    if (buttons & (1 << 1)) {
        event = kmalloc(sizeof(*event), GFP_ATOMIC);
        if (event) {
            snprintf(event->message, sizeof(event->message),
                     "Right Button Pressed\n");
            INIT_WORK(&event->work, mouse_event_worker);
            queue_work(mouse_wq, &event->work);
            printk(KERN_INFO "mouse_raw_event: Queued right button event.\n");
        }
    }

    if (buttons & (1 << 2)) {
        event = kmalloc(sizeof(*event), GFP_ATOMIC);
        if (event) {
            snprintf(event->message, sizeof(event->message),
                     "Middle Button Pressed\n");
            INIT_WORK(&event->work, mouse_event_worker);
            queue_work(mouse_wq, &event->work);
            printk(KERN_INFO "mouse_raw_event: Queued middle button event.\n");
        }
    }
    return 0;		
}

// HID driver structure.
static struct hid_driver mouse_hid_driver = {
    .name = "mouse_driver",
    .id_table = mouse_hid_table,
    .probe = mouse_usb_probe,
    .remove = mouse_usb_remove,
    .raw_event = mouse_raw_event,
};
//-----------------------------------------------------------------------------------------------------------------------------------------------------------------

/*
 * mouse_init registers the HID driver.
 */
static int __init mouse_init(void)
{
    int hid_result;

		mouse_wq = alloc_workqueue("mouse_wq", WQ_UNBOUND, 0);
    if (!mouse_wq) {
        printk(KERN_ERR "Failed to create workqueue\n");
        return -ENOMEM;
    }

    hid_result = hid_register_driver(&mouse_hid_driver);
    if (hid_result) {
        printk(KERN_ALERT "USB driver registration failed.\n");
        return -EFAULT;
    }
		printk(KERN_INFO "Mouse driver initialised with workqueue\n");
    return 0;
}

/*
 * mouse_exit unregisters the HID driver.
 */
static void __exit mouse_exit(void)
{
		flush_workqueue(mouse_wq);
    destroy_workqueue(mouse_wq);
    printk(KERN_INFO "Mouse driver exit: workqueue destroyed\n");
    hid_unregister_driver(&mouse_hid_driver);
    printk(KERN_INFO "Mouse device unregistered\n");
}

module_init(mouse_init);
module_exit(mouse_exit);

