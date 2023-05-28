#include <linux/ioctl.h>
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
#include <linux/pagewalk.h>
#include <linux/pagemap.h>
#include <linux/shmem_fs.h>
#include <linux/kallsyms.h>
#include <linux/compiler-gcc.h>
#include <linux/version.h>
#include "module.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("funera1");

#define DRIVER_NAME "rss_range"
#define DRIVER_MAJOR 64

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
#define KPROBE_LOOKUP 1
#include <linux/kprobes.h>
static struct kprobe kp = {
    .symbol_name = "kallsyms_lookup_name"
};
#endif

#define walk_page_range walk_page_range_ptr
#define smaps_pte_hole smaps_pte_hole_ptr
#define smaps_pte_range smaps_pte_range_ptr
#define smaps_hugetlb_range smaps_hugetlb_range_ptr
#define shmem_swap_usage shmem_swap_usage_ptr

static int resolve_non_exported_symbols(void)
{
#ifdef KPROBE_LOOKUP
    typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
    kallsyms_lookup_name_t kallsyms_lookup_name;
    register_kprobe(&kp);
    kallsyms_lookup_name = (kallsyms_lookup_name_t) kp.addr;
    unregister_kprobe(&kp);
#endif
    walk_page_range_ptr = 
        (walk_page_range_t)kallsyms_lookup_name("walk_page_range");
    smaps_pte_hole_ptr = 
        (smaps_pte_hole_t)kallsyms_lookup_name("smaps_pte_hole");
    smaps_pte_range_ptr = 
        (smaps_pte_range_t)kallsyms_lookup_name("smaps_pte_range");
    smaps_hugetlb_range_ptr = 
        (smaps_hugetlb_range_t)kallsyms_lookup_name("smaps_hugetlb_range");
    shmem_swap_usage_ptr = 
        (shmem_swap_usage_t)kallsyms_lookup_name("shmem_swap_usage");

    if (!walk_page_range_ptr || !smaps_pte_hole_ptr || !smaps_pte_range_ptr || !smaps_hugetlb_range_ptr || !shmem_swap_usage_ptr) {
        return -ENOENT;
    }

    return 0;
}

static void my_smap_gather_stats(struct vm_area_struct *vma,
        struct mem_size_stats *mss, unsigned long start, unsigned long end)
{
    printk("Start my_smap_gather_stats\n");
    struct mm_walk_ops smaps_walk_ops = {
        .pmd_entry		= smaps_pte_range_ptr,
        .hugetlb_entry		= smaps_hugetlb_range_ptr,
    };
	const struct mm_walk_ops *ops = &smaps_walk_ops;
    printk("Set smaps_walk_ops\n");

    printk("vma->vm_start = %lu\n", vma->vm_start);
    printk("vma->vm_end = %lu\n", vma->vm_end);
	/* Invalid start */
	if (start >= vma->vm_end) {
        printk("start >= vma->vm_end\n");
		return;
    }

    printk("Before Inside CONFIG_SHMEM\n");
#ifdef CONFIG_SHMEM
    printk("Inside CONFIG_SHMEM\n");
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
        printk("Set shmem_swapped\n");
		unsigned long shmem_swapped = shmem_swap_usage(vma);

		if (!start && (!shmem_swapped || (vma->vm_flags & VM_SHARED) ||
					!(vma->vm_flags & VM_WRITE))) {
			mss->swap += shmem_swapped;
		} else {
            const struct mm_walk_ops smaps_shmem_walk_ops = {
                .pmd_entry		= smaps_pte_range,
                .hugetlb_entry		= smaps_hugetlb_range,
                .pte_hole		= smaps_pte_hole,
            };
			ops = &smaps_shmem_walk_ops;
		}
	}
#endif
	/* mmap_lock is held in m_start */
    // TODO: start, endの範囲チェック
    if (end > vma->vm_end)
        end = vma->vm_end;
    printk("Start walk_page_range\n");
    walk_page_range(vma->vm_mm, start, end, ops, mss);
    printk("End walk_page_range\n");
}

/*
 * ==============================================================================================================
 * pid, addr_start, addr_endから任意アドレス空間のrssを取得する
 */
static long module_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) 
{
    struct module_values* val = (struct module_values *)arg;

    // pidからvmaを取得
    printk("finding pid %d's task\n", val->pid);
    struct pid* pid = find_get_pid(val->pid);
    if (!pid) {
        printk("couldn't find pid %d's task\n", val->pid);
        return -1;
    }

    printk("getting task\n", val->pid);
    struct task_struct* task = get_pid_task(pid, PIDTYPE_PID);
    if (!task) {
        printk("couldn't get task\n");
        return -1;
    }

    // 任意アドレス空間rss取得
    // NOTE: ここらへんで壊れてそう
    // struct vm_area_struct* vma = task->mm->mmap_base;
    struct vm_area_struct* vma = find_vma(task->mm, val->addr_start);
    if (!vma) {
        printk("The vma is not found\n");
        return -1;
    }
    printk("Found vma: ");
    printk("%lu\n", vma->vm_start);
    struct mem_size_stats mss;

    memset(&mss, 0, sizeof(mss));

    my_smap_gather_stats(vma, &mss, val->addr_start, val->addr_end);
    return mss.resident;
}

static struct file_operations module_fops = {
  .owner   = THIS_MODULE,
  .unlocked_ioctl = module_ioctl
};

// static dev_t dev_id;
// static struct cdev c_dev;

static int __init module_initialize(void)
{
    printk("Init rss_range\n");
    int res = resolve_non_exported_symbols();
    if (res) {
        return -ENOENT;
    }

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
    printk("Exit rss_range\n");
    unregister_chrdev(DRIVER_MAJOR, DRIVER_NAME);
  // cdev_del(&c_dev);
  // unregister_chrdev_region(dev_id, 1);
}

module_init(module_initialize);
module_exit(module_cleanup);
