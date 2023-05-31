#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/compiler.h>
#include "module.h"

// static void hold_task_mempolicy(struct task_struc *task)
// {
//     task_lock(task);
//     task_mempolicy = get_task_policy(task);
//     mpol_get(task_mempolicy);
//     task_unlock(task);
// }
// 
// static void release_task_mempolicy(task)
// {
// 
// }

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
	/* mmap_lock is held in m_start */
	// if (!start)
	// 	walk_page_vma(vma, ops, mss);
	// else
    if (end > vma->vm_end)
        end = vma->vm_end;
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

    // TODO: ここの意味考える & gotoの処理考える
    if(!mm || !mmget_not_zero(mm)) {
        ret = -ESRCH;
        return ret;
    }

    ret = mmap_read_lock_killable(mm);
    // TODO: gotoの処理考える
    if (ret)
        goto out_put_mm;

    // TODO: task_mempolicyの扱いをどうするか
    vma = vma_next(&vmi);

    // TODO: unlikely
    if (unlikely(!vma))
        goto empty_set;
    
    vma_start = start;
    do {
        smap_gather_stats_range(vma, mss, vma_start, end);
        last_vma_end = vma->vm_end;

        if (mmap_lock_is_contended(mm)) {
            // TODO: この関数何か調べる
            vma_iter_invalidate(&vmi);
            mmap_read_unlock(mm);
            ret = mmap_read_lock_killable(mm);
            if (ret) {
                // TODO: gotoを考える
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
            if (!vma)
                break;

            /* Case 1 and 2 above */
            if (vma->vm_start >= last_vma_end)
                continue;

            /* Case 4 above */
            if (vma->vm_end > last_vma_end) {
                // TODO: start, endの範囲について考える
                smap_gather_stats_range(vma, mss, last_vma_end, end);
            }
        }
    } for_each_vma_range(vmi, vma, end);


empty_set:
    __show_smap(mss);
	mmap_read_unlock(mm);

out_put_mm:
	mmput(mm);

	return ret;
}
