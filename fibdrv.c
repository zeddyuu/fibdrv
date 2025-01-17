#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

MODULE_LICENSE("Dual MIT/GPL");
MODULE_AUTHOR("National Cheng Kung University, Taiwan");
MODULE_DESCRIPTION("Fibonacci engine driver");
MODULE_VERSION("0.1");

#define DEV_FIBONACCI_NAME "fibonacci"

/* MAX_LENGTH is set to 92 because
 * ssize_t can't fit the number > 92
 */

#define MAX_LENGTH 500

typedef struct strbuf {
    char fib[128];
};



static dev_t fib_dev = 0;
static struct cdev *fib_cdev;
static struct class *fib_class;
static DEFINE_MUTEX(fib_mutex);

static long long fib_sequence(long long k)
{
    /* FIXME: C99 variable-length array (VLA) is not allowed in Linux kernel. */
    if (k < 2)
        return k;
    long long a = 0, b = 1;
    int len_zero = __builtin_clzll(k), counter = 64 - len_zero;
    k <<= len_zero;

    for (; counter; k <<= 1, counter--) {
        long long t1 = a * (2 * b - a);
        long long t2 = b * b + a * a;
        a = t1, b = t2;
        if (k & 1ULL << 63) {
            t1 = a + b;
            a = b;
            b = t1;
        }
    }
    return a;
}


static void add_string(char *x, char *y, char *next)
{
    int x_size = strlen(x), y_size = strlen(y);
    int i, carry = 0;
    int sum;
    if (x_size >= y_size) {
        for (i = 0; i < y_size; i++) {
            sum = (x[i] - '0') + (y[i] - '0') + carry;
            next[i] = '0' + sum % 10;
            carry = sum / 10;
        }
        for (i = y_size; i < x_size; i++) {
            sum = (x[i] - '0') + carry;
            next[i] = '0' + sum % 10;
            carry = sum / 10;
        }
    } else {
        for (i = 0; i < x_size; i++) {
            sum = (x[i] - '0') + (y[i] - '0') + carry;
            next[i] = '0' + sum % 10;
            carry = sum / 10;
        }
        for (i = x_size; i < y_size; i++) {
            sum = (y[i] - '0') + carry;
            next[i] = '0' + sum % 10;
            carry = sum / 10;
        }
    }
    if (carry) {
        next[i] = '1';
    }
    next[++i] = '\0';
}

void reverse_string(char *str, size_t size)
{
    for (int i = 0; i < size - i; i++) {
        str[i] = str[i] ^ str[size - i];
        str[size - i] = str[i] ^ str[size - i];
        str[i] = str[i] ^ str[size - i];
    }
}

static long long fib_sequence_string(long long k, char *buf)
{
    struct strbuf *res = kmalloc(sizeof(struct strbuf) * (k + 1), GFP_KERNEL);
    strncpy(res[0].fib, "0", 2);
    strncpy(res[1].fib, "1", 2);
    for (int i = 2; i <= k; i++) {
        add_string(res[i - 1].fib, res[i - 2].fib, res[i].fib);
    }
    size_t size = strlen(res[k].fib);
    reverse_string(res[k].fib, size - 1);
    __copy_to_user(buf, res[k].fib, size + 1);
    return size;
}



static int fib_open(struct inode *inode, struct file *file)
{
    if (!mutex_trylock(&fib_mutex)) {
        printk(KERN_ALERT "fibdrv is in use");
        return -EBUSY;
    }
    return 0;
}

static int fib_release(struct inode *inode, struct file *file)
{
    mutex_unlock(&fib_mutex);
    return 0;
}

/* calculate the fibonacci number at given offset */
static ssize_t fib_read(struct file *file,
                        char *buf,
                        size_t size,
                        loff_t *offset)
{
    return (ssize_t) fib_sequence_string(*offset, buf);
}

/* write operation is skipped */
static ssize_t fib_write(struct file *file,
                         const char *buf,
                         size_t size,
                         loff_t *offset)
{
    return 1;
}

static loff_t fib_device_lseek(struct file *file, loff_t offset, int orig)
{
    loff_t new_pos = 0;
    switch (orig) {
    case 0: /* SEEK_SET: */
        new_pos = offset;
        break;
    case 1: /* SEEK_CUR: */
        new_pos = file->f_pos + offset;
        break;
    case 2: /* SEEK_END: */
        new_pos = MAX_LENGTH - offset;
        break;
    }

    if (new_pos > MAX_LENGTH)
        new_pos = MAX_LENGTH;  // max case
    if (new_pos < 0)
        new_pos = 0;        // min case
    file->f_pos = new_pos;  // This is what we'll use now
    return new_pos;
}

const struct file_operations fib_fops = {
    .owner = THIS_MODULE,
    .read = fib_read,
    .write = fib_write,
    .open = fib_open,
    .release = fib_release,
    .llseek = fib_device_lseek,
};

static int __init init_fib_dev(void)
{
    int rc = 0;

    mutex_init(&fib_mutex);

    // Let's register the device
    // This will dynamically allocate the major number
    rc = alloc_chrdev_region(&fib_dev, 0, 1, DEV_FIBONACCI_NAME);

    if (rc < 0) {
        printk(KERN_ALERT
               "Failed to register the fibonacci char device. rc = %i",
               rc);
        return rc;
    }

    fib_cdev = cdev_alloc();
    if (fib_cdev == NULL) {
        printk(KERN_ALERT "Failed to alloc cdev");
        rc = -1;
        goto failed_cdev;
    }
    fib_cdev->ops = &fib_fops;
    rc = cdev_add(fib_cdev, fib_dev, 1);

    if (rc < 0) {
        printk(KERN_ALERT "Failed to add cdev");
        rc = -2;
        goto failed_cdev;
    }

    fib_class = class_create(THIS_MODULE, DEV_FIBONACCI_NAME);

    if (!fib_class) {
        printk(KERN_ALERT "Failed to create device class");
        rc = -3;
        goto failed_class_create;
    }

    if (!device_create(fib_class, NULL, fib_dev, NULL, DEV_FIBONACCI_NAME)) {
        printk(KERN_ALERT "Failed to create device");
        rc = -4;
        goto failed_device_create;
    }
    return rc;
failed_device_create:
    class_destroy(fib_class);
failed_class_create:
    cdev_del(fib_cdev);
failed_cdev:
    unregister_chrdev_region(fib_dev, 1);
    return rc;
}

static void __exit exit_fib_dev(void)
{
    mutex_destroy(&fib_mutex);
    device_destroy(fib_class, fib_dev);
    class_destroy(fib_class);
    cdev_del(fib_cdev);
    unregister_chrdev_region(fib_dev, 1);
}

module_init(init_fib_dev);
module_exit(exit_fib_dev);
