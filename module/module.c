#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/shmem_fs.h>
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
#include <linux/kallsyms.h>
#include <linux/compiler-gcc.h>
#include <linux/version.h>
#include "module.h"

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("funera1");

#define DEVICE_NAME "rss_range"
#define DEVICE_MAJOR 64

#define RSS_RANGE 0
#define NEXT_VMA_PTR 1

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
#define KPROBE_LOOKUP 1
#include <linux/kprobes.h>
static struct kprobe kp = {
    .symbol_name = "kallsyms_lookup_name"
};
#endif


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
    vma_iter_invalidate_ptr = 
        (vma_iter_invalidate_t)kallsyms_lookup_name("vma_iter_invalidate");

    if (!walk_page_range_ptr || !smaps_pte_hole_ptr || !smaps_pte_range_ptr || !smaps_hugetlb_range_ptr || !shmem_swap_usage_ptr || vma_iter_invalidate_ptr) {
        return -ENOENT;
    }

    return 0;
}



/*
 * ==============================================================================================================
 * pid, addr_start, addr_endから任意アドレス空間のrssを取得する
 */
static long module_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) 
{
    printk("Start module_ioctl\n");
    struct module_values* val = (struct module_values *)arg;

    // pidからvmaを取得
    struct pid* pid = find_get_pid(val->pid);
    if (!pid) {
        printk("Couldn't find pid %d's task\n", val->pid);
        return -1;
    }

    printk("Start get pid task\n");
    struct task_struct* task = get_pid_task(pid, PIDTYPE_PID);
    if (!task) {
        printk("Couldn't get task\n");
        return -1;
    }

    struct mem_size_stats mss;
    memset(&mss, 0, sizeof(mss));

    printk("Start get_smaps_range\n");
    int ret = get_smaps_range(task, &mss, val->addr_start, val->addr_end);
    put_task_struct(task);

    if (ret) {
        printk("Couldn't get smaps range\n");
        return -1;
    }

    // 任意アドレス空間rss取得
    // struct vm_area_struct* vma = find_vma(task->mm, val->addr_start);
    // if (!vma) {
    //     printk("The vma is not found\n");
    //     return -1;
    // }
    // printk("%lu\n", vma->vm_start);

    // smap_gather_stats_range(vma, &mss, val->addr_start, val->addr_end);
    __show_smap(&mss);

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

  register_chrdev(DRIVER_MAJOR, DRIVER_NAME, &module_fops);
  return 0;
}

static void __exit module_cleanup(void)
{
    printk("Exit rss_range\n");
    unregister_chrdev(DRIVER_MAJOR, DRIVER_NAME);
}

module_init(module_initialize);
module_exit(module_cleanup);


// smaps.c

void __show_smap(const struct mem_size_stats *mss)
{
#define PSS_SHIFT 12
    printk("Rss: %lukB\n", mss->resident);
    printk("Pss: %lukB\n", mss->pss >> PSS_SHIFT);
    printk("Pss_Dirty: %lukB\n", mss->pss_dirty >> PSS_SHIFT);
    printk("Anonymous: %lukB\n", mss->anonymous);
#undef PSS_SHIFT
}

void smap_gather_stats_range(struct vm_area_struct *vma,
        struct mem_size_stats *mss, unsigned long start, unsigned long end)
{
    struct mm_walk_ops smaps_walk_ops = {
        .pmd_entry		= smaps_pte_range_ptr,
        .hugetlb_entry		= smaps_hugetlb_range_ptr,
    };
	const struct mm_walk_ops *ops = &smaps_walk_ops;
	/* Invalid start */
	if (start >= vma->vm_end) {
        printk("start >= vma->vm_end\n");
		return;
    }

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
            const struct mm_walk_ops smaps_shmem_walk_ops = {
                .pmd_entry		= smaps_pte_range,
                .hugetlb_entry		= smaps_hugetlb_range,
                .pte_hole		= smaps_pte_hole,
            };
			ops = &smaps_shmem_walk_ops;
		}
	}
#endif
    if (end > vma->vm_end)
        end = vma->vm_end;

    printk("Search: [%lu, %lu]\n", start, end);
    walk_page_range(vma->vm_mm, start, end, ops, mss);
}

int get_smaps_range(struct task_struct* task, struct mem_size_stats* mss, unsigned long start, unsigned long end)
{
    if(!task)
        return -ESRCH;

    struct mm_struct *mm = task->mm;
    struct vm_area_struct *vma;
    unsigned long vma_start = 0, last_vma_end = 0;
    int ret = 0;
    VMA_ITERATOR(vmi, mm, start);

    if(!mm || !mmget_not_zero(mm)) {
        ret = -ESRCH;
        return ret;
    }

    ret = mmap_read_lock_killable(mm);
    if (ret)
        goto out_put_mm;

    vma = vma_next(&vmi);

    if (unlikely(!vma))
        goto empty_set;
    
    vma_start = start;
    do {
        smap_gather_stats_range(vma, mss, vma_start, end);
        last_vma_end = vma->vm_end;
        printk("[%lu, %lu]\n", vma_start, last_vma_end);

        if (mmap_lock_is_contended(mm)) {
            // TODO: この関数何か調べる
            vma_iter_invalidate(&vmi);
            mmap_read_unlock(mm);
            ret = mmap_read_lock_killable(mm);
            if (ret) {
                // TODO: gotoを考える
                printk("Not mmap_read_lock_killable\n");
                goto out_put_mm;
            }

			/*
			 * After dropping the lock, there are four cases to
			 * consider. See the following example for explanation.
			 *
			 *   +------+------+-----------+
			 *   | VMA1 | VMA2 | VMA3      |
			 *   +------+------+-----------+
			 *   |      |      |           |
			 *  4k     8k     16k         400k
			 *
			 * Suppose we drop the lock after reading VMA2 due to
			 * contention, then we get:
			 *
			 *	last_vma_end = 16k
			 *
			 * 1) VMA2 is freed, but VMA3 exists:
			 *
			 *    vma_next(vmi) will return VMA3.
			 *    In this case, just continue from VMA3.
			 *
			 * 2) VMA2 still exists:
			 *
			 *    vma_next(vmi) will return VMA3.
			 *    In this case, just continue from VMA3.
			 *
			 * 3) No more VMAs can be found:
			 *
			 *    vma_next(vmi) will return NULL.
			 *    No more things to do, just break.
			 *
			 * 4) (last_vma_end - 1) is the middle of a vma (VMA'):
			 *
			 *    vma_next(vmi) will return VMA' whose range
			 *    contains last_vma_end.
			 *    Iterate VMA' from last_vma_end.
			 */
            vma = vma_next(&vmi);
            /* Case 3 above */
            if (!vma) {
                printk("!vma\n");
                break;
            }

            /* Case 1 and 2 above */
            if (vma->vm_start >= last_vma_end) {
                printk("vma->vm_start >= last_vma_end\n");
                continue;
            }

            /* Case 4 above */
            if (vma->vm_end > last_vma_end) {
                smap_gather_stats_range(vma, mss, last_vma_end, end);
            }
        }
    } for_each_vma_range(vmi, vma, end);
printk("End for_each_vma_range\n");

empty_set:
    __show_smap(mss);
	mmap_read_unlock(mm);

out_put_mm:
	mmput(mm);

	return ret;
}
