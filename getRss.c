#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/pid.h>
#include "getRss.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("funera1");

#define DRIVER_NAME "getRss"
#define DRIVER_MAJOR 64

static int module_open(struct inode *inode, struct file *filp) {
  printk("'module_open' called\n");
  return 0;
}

static int module_close(struct inode *inode, struct file *filp) {
  printk("'module_close' called\n");
  return 0;
}

/*
 * ==============================================================================================================
 * pid, addr_start, addr_endから任意アドレス空間のrssを取得する
 */
static long module_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) 
{
    struct module_values* val = (struct module_values *)arg;
    struct pid* pid = find_get_pid(val->pid);
    if (!pid) {
        printk("couldn't find pid %d's task\n", val->pid);
        return -1;
    }
    struct task_struct* task = get_pid_task(pid, PIDTYPE_PID);
    if (!task) {
        printk("couldn't get task\n");
        return -1;
    }
    struct vm_area_struct* vma = task->mm->mmap_base;
    return 0;
}

static struct file_operations module_fops = {
  .owner   = THIS_MODULE,
  .open    = module_open,
  .release = module_close,
  .unlocked_ioctl = module_ioctl
};

// static dev_t dev_id;
// static struct cdev c_dev;

static int __init module_initialize(void)
{
    printk("GetRss_init\n");
    /* ★ カーネルに、本ドライバを登録する */
    register_chrdev(DRIVER_MAJOR, DRIVER_NAME, &module_fops);
    return 0;
  // if (alloc_chrdev_region(&dev_id, 0, 1, DEVICE_NAME))
  //   return -EBUSY;
  // 
  // cdev_init(&c_dev, &module_fops);
  // c_dev.owner = THIS_MODULE;
  // 
  // if (cdev_add(&c_dev, dev_id, 1)) {
  //   unregister_chrdev_region(dev_id, 1);
  //   return -EBUSY;
  // }

  return 0;
}

static void __exit module_cleanup(void)
{
    printk("GetRss_exit\n");
    unregister_chrdev(DRIVER_MAJOR, DRIVER_NAME);
  // cdev_del(&c_dev);
  // unregister_chrdev_region(dev_id, 1);
}

module_init(module_initialize);
module_exit(module_cleanup);
