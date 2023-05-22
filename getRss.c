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
#include <linux/task_mmu.c>
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

static void my_smap_gather_stats(struct vm_area_struct *vma,
        struct mem_size_stats *mss, unsigned long start, unsigned long end)
{
	const struct mm_walk_ops *ops = &smaps_walk_ops;

	/* Invalid start */
	if (start >= vma->vm_end)
		return;

#ifdef CONFIG_SHMEM
	if (vma->vm_file && shmem_mapping(vma->vm_file->f_mapping)) {
		/*
		 * For shared or readonly shmem mappings we know that all
		 * swapped out pages belong to the shmem object, and we can
		 * obtain the swap value much more efficiently. For private
		 * writable mappings, we might have COW pages that are
		 * not affected by the parent swapped out pages of the shmem
		 * object, so we have to distinguish them during the page walk.
		 * Unless we know that the shmem object (or the part mapped by
		 * our VMA) has no swapped out pages at all.
		 */
		unsigned long shmem_swapped = shmem_swap_usage(vma);

		if (!start && (!shmem_swapped || (vma->vm_flags & VM_SHARED) ||
					!(vma->vm_flags & VM_WRITE))) {
			mss->swap += shmem_swapped;
		} else {
			ops = &smaps_shmem_walk_ops;
		}
	}
#endif
	/* mmap_lock is held in m_start */
    walk_page_range(vma->vm_mm, start, end, ops, mss);
}

/*
 * ==============================================================================================================
 * pid, addr_start, addr_endから任意アドレス空間のrssを取得する
 */
static long module_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) 
{
    struct module_values* val = (struct module_values *)arg;

    // pidからvmaを取得
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

    // 任意アドレス空間rss取得
    struct vm_area_struct* vma = task->mm->mmap_base;
    struct mem_size_stats mss;

    memset(&mss, 0, sizeof(mss));

    my_smap_gather_stats(vma, &mss, val->addr_start, val->addr_end);
    return mss->resident;
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
