#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "wacom_tablet"
#define CLASS_NAME "tablet"
#define BUF_SIZE 1024  

#define DEVICE_VENDOR_ID 0x056a
#define DEVICE_PRODUCT_ID 0x0357

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yasmin, David, Waleed and April");
MODULE_DESCRIPTION("Wacom tablet driver with URB-based asynchronous reads and numbered buttons.");
MODULE_VERSION("1.0");


static int major;
static struct class *tablet_class = NULL;
static struct device *tablet_device = NULL;
static struct cdev tablet_cdev;

static char *msg_buffer;    // buffer to store messages
static int buffer_offset = 0;  // current length of data in the buffer
static DEFINE_MUTEX(buffer_mutex);  // protects our buffer

/*
 * tablet_read - Called when a userspace process reads from our device node.
 * It copies data from the internal buffer to the user-space buffer.
 */
static ssize_t tablet_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
    ssize_t ret;

    mutex_lock(&buffer_mutex);
    if (*offset >= buffer_offset) {
        mutex_unlock(&buffer_mutex);
        return 0;
    }
    if (count > (buffer_offset - *offset))
        count = buffer_offset - *offset;
    ret = copy_to_user(buf, msg_buffer + *offset, count);
    if (ret) {
        mutex_unlock(&buffer_mutex);
        return -EFAULT;
    }
    *offset += count;
    mutex_unlock(&buffer_mutex);
    return count;
}

/*
 * tablet_open - Called when the device node is opened.
 */
static int tablet_open(struct inode *inode, struct file *file)
{
    return 0;
}

/*
 * tablet_release - Called when the device node is closed.
 */
static int tablet_release(struct inode *inode, struct file *file)
{
    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = tablet_read,
    .open = tablet_open,
    .release = tablet_release,
};

/*
 * clear_buffer - Clears the internal message buffer.
 * This function is called when the buffer is about to overflow.
 */
static void clear_buffer(void)
{
    mutex_lock(&buffer_mutex);
    buffer_offset = 0;
    memset(msg_buffer, 0, BUF_SIZE);
    mutex_unlock(&buffer_mutex);
}

/*
 * simulate_button_press - Simulates a button press event from the drawing tablet.
 * In a real driver, this would be called by the hardware event handler.
 * The function appends "button X pressed" to the internal buffer.
 */
static void simulate_button_press(int button)
{
    char temp[64];
    int len;

    mutex_lock(&buffer_mutex);
    len = snprintf(temp, sizeof(temp), "button %d pressed\n", button);
    // If adding the new message would exceed our buffer, clear it first.
    if (buffer_offset + len >= BUF_SIZE)
        clear_buffer();
    memcpy(msg_buffer + buffer_offset, temp, len);
    buffer_offset += len;
    mutex_unlock(&buffer_mutex);
}

/*
 * tablet_init - Module initialisation.
 * This function sets up the character device, allocates memory for the buffer,
 * and simulates a few button presses.
 */
static int __init tablet_init(void)
{
    int ret;
    dev_t dev;

    // Allocate memory for our message buffer.
    msg_buffer = kmalloc(BUF_SIZE, GFP_KERNEL);
    if (!msg_buffer)
        return -ENOMEM;
    memset(msg_buffer, 0, BUF_SIZE);

    // Dynamically allocate a major number for the device.
    ret = alloc_chrdev_region(&dev, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        kfree(msg_buffer);
        return ret;
    }
    major = MAJOR(dev);

    // Initialise and add our character device.
    cdev_init(&tablet_cdev, &fops);
    tablet_cdev.owner = THIS_MODULE;
    ret = cdev_add(&tablet_cdev, dev, 1);
    if (ret < 0) {
        unregister_chrdev_region(dev, 1);
        kfree(msg_buffer);
        return ret;
    }

    // Create a device class and device node in /dev.
    tablet_class = class_create("wacom_tablet_class");
    if (IS_ERR(tablet_class)) {
        cdev_del(&tablet_cdev);
        unregister_chrdev_region(dev, 1);
        kfree(msg_buffer);
        return PTR_ERR(tablet_class);
    }
    tablet_device = device_create(tablet_class, NULL, dev, NULL, DEVICE_NAME);
    if (IS_ERR(tablet_device)) {
        class_destroy(tablet_class);
        cdev_del(&tablet_cdev);
        unregister_chrdev_region(dev, 1);
        kfree(msg_buffer);
        return PTR_ERR(tablet_device);
    }

    // For demonstration, simulate a few button press events.
    simulate_button_press(1);
    simulate_button_press(2);
    simulate_button_press(3);

    printk(KERN_INFO "Tablet buttons driver initialised with major %d\n", major);
    return 0;
}

/*
 * tablet_exit - Module cleanup.
 * This function destroys the device node, class, and releases resources.
 */
static void __exit tablet_exit(void)
{
    dev_t dev = MKDEV(major, 0);
    device_destroy(tablet_class, dev);
    class_destroy(tablet_class);
    cdev_del(&tablet_cdev);
    unregister_chrdev_region(dev, 1);
    kfree(msg_buffer);
    printk(KERN_INFO "Tablet buttons driver exited\n");
}

module_init(tablet_init);
module_exit(tablet_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("A simple input device driver for drawing tablet buttons");
