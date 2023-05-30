#include <sched.h>
#include <mm.h>

static void hold_task_mempolicy(struct task_struc *task)
{
    task_lock(task);
    task_mempolicy = get_task_policy(task);
    mpol_get(task_mempolicy);
    task_unlock(task);
}

static void release_task_mempolicy(task)
{

}

static int get_smaps_range(struct task_struct* task, unsigned long start, unsigned long end)
{
    if(!task)
        return -ESRCH;

    struct mem_size_stats mss;
    struct mm_struct *mm = task->mm;
    struct vm_area_struct *vma;
    unsigned long vma_start = 0, last_vma_end = 0;
    int ret = 0;

    // TODO: ここの意味考える & gotoの処理考える
    if(!mm || !mmget_not_zero(mm)) {
        ret = -ESRCH;
        goto out_put_task;
    }

    memset(&mss, 0, sizeof(mss));

    ret = mmap_read_lock_killable(mm);
    // TODO: gotoの処理考える
    if (ret)
        goto out_put_mm;

    // TODO: task_mempolicyの扱いをどうするか
    hold_task_mempolicy(task);
    vma = find_vma(mm, start);

    // TODO: unlikely
    if (unlikely(!vma))
        goto empty_set;
    
    vma_start = start;
    do {
        my_smap_gather_stats(vma, &mss, vma_start, end);
        last_vma_end = vma->vm_end;

        // TODO: この関数何か調べる
        if (mmap_lock_is_contended(mm)) {
            // TODO: この関数何か調べる
            vma_iter_invalidate();
            // TODO: この関数何か調べる
            mmap_read_unlock(mm);
            ret = mmap_read_lock_killable(mm);
            if (ret) {
                // TODO: releaseの中身考える
                release_task_mempolicy(task);
                // TODO: gotoを考える
                goto out_put_mm;
            }

            // TODO: これが言ってることちゃんと理解する
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
            vma = vma_next();
            /* Case 3 above */
            if (!vma)
                break;

            /* Case 1 and 2 above */
            if (vma->vm_start >= last_vma_end)
                continue;

            /* Case 4 above */
            if (vma->vm_end > last_vma_end) {
                // TODO: start, endの範囲について考える
                my_smap_gather_stats(vma, &mss, last_vma_end);
            }
        }
    } for_each_vma();
}
