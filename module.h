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
#include <linux/mempolicy.h>
#include <linux/pid.h>
#include <linux/pagewalk.h>
#include <linux/pagemap.h>
#include <linux/kallsyms.h>
#include <linux/compiler-gcc.h>

typedef unsigned long long u64;
// walk_page_range
typedef int (*walk_page_range_t)(struct mm_struct *mm, unsigned long start,
        unsigned long end, const struct mm_walk_ops *ops, void *private);
walk_page_range_t walk_page_range_ptr;
// smaps_pte_hole
typedef int (*smaps_pte_hole_t)(unsigned long addr, unsigned long end,
			  __always_unused int depth, struct mm_walk *walk);
smaps_pte_hole_t smaps_pte_hole_ptr;
// smaps_pte_range
typedef int (*smaps_pte_range_t)(pmd_t *pmd, unsigned long addr, unsigned long end,
			   struct mm_walk *walk);
smaps_pte_range_t smaps_pte_range_ptr;
// smaps_hugetlb_range
typedef int (*smaps_hugetlb_range_t)(pte_t *pte, unsigned long hmask,
				 unsigned long addr, unsigned long end,
				 struct mm_walk *walk);
smaps_hugetlb_range_t smaps_hugetlb_range_ptr;

static struct module_values {
    pid_t pid;
    unsigned long addr_start;
    unsigned long addr_end;
};

struct mem_size_stats {
	unsigned long resident;
	unsigned long shared_clean;
	unsigned long shared_dirty;
	unsigned long private_clean;
	unsigned long private_dirty;
	unsigned long referenced;
	unsigned long anonymous;
	unsigned long lazyfree;
	unsigned long anonymous_thp;
	unsigned long shmem_thp;
	unsigned long file_thp;
	unsigned long swap;
	unsigned long shared_hugetlb;
	unsigned long private_hugetlb;
	u64 pss;
	u64 pss_anon;
	u64 pss_file;
	u64 pss_shmem;
	u64 pss_dirty;
	u64 pss_locked;
	u64 swap_pss;
};

