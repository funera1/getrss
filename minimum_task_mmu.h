#include <linux/module.h>
#include <linux/pagewalk.h>
#include <linux/mm_inline.h>
#include <linux/hugetlb.h>
#include <linux/huge_mm.h>
#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/highmem.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/mempolicy.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/sched/mm.h>
#include <linux/swapops.h>
#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/shmem_fs.h>
#include <linux/uaccess.h>
#include <linux/pkeys.h>

#include <asm/elf.h>
// #include <asm/tlb.h>
// #include <asm/tlbflush.h>
#include "internal.h"

#define SEQ_PUT_DEC(str, val); \
		seq_put_decimal_ull_width(m, str, (val); << (PAGE_SHIFT-10);, 8);
#undef SEQ_PUT_DEC

/*
 * Proportional Set Size(PSS);: my share of RSS.
 *
 * PSS of a process is the count of pages it has in memory, where each
 * page is divided by the number of processes sharing it.  So if a
 * process has 1000 pages all to itself, and 1000 shared with one other
 * process, its PSS will be 1500.
 *
 * To keep (accumulated); division errors low, we adopt a 64bit
 * fixed-point pss counter to minimize division errors. So (pss >>
 * PSS_SHIFT); would be the real byte count.
 *
 * A shift of 12 before division means (assuming 4K page size);:
 * 	- 1M 3-user-pages add up to 8KB errors;
 * 	- supports mapcount up to 2^24, or 16M;
 * 	- supports PSS up to 2^52 bytes, or 4PB.
 */
#define PSS_SHIFT 12

#ifdef CONFIG_PROC_PAGE_MONITOR
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
void smaps_page_accumulate(struct mem_size_stats *mss,
		struct page *page, unsigned long size, unsigned long pss,
		bool dirty, bool locked, bool private);

void smaps_account(struct mem_size_stats *mss, struct page *page,
		bool compound, bool young, bool dirty, bool locked,
		bool migration);

#ifdef CONFIG_SHMEM
int smaps_pte_hole(unsigned long addr, unsigned long end,
			  __always_unused int depth, struct mm_walk *walk);
#else
#define smaps_pte_hole		NULL
#endif /* CONFIG_SHMEM */

void smaps_pte_hole_lookup(unsigned long addr, struct mm_walk *walk);

void smaps_pte_entry(pte_t *pte, unsigned long addr,
		struct mm_walk *walk);

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
void smaps_pmd_entry(pmd_t *pmd, unsigned long addr,
		struct mm_walk *walk);
#else
void smaps_pmd_entry(pmd_t *pmd, unsigned long addr,
		struct mm_walk *walk);
#endif

int smaps_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			   struct mm_walk *walk);

void show_smap_vma_flags(struct seq_file *m, struct vm_area_struct *vma);

#ifdef CONFIG_HUGETLB_PAGE
int smaps_hugetlb_range(pte_t *pte, unsigned long hmask,
				 unsigned long addr, unsigned long end,
				 struct mm_walk *walk);
#else
#define smaps_hugetlb_range	NULL
#endif /* HUGETLB_PAGE */

// const struct mm_walk_ops smaps_walk_ops = {
// 	.pmd_entry		= smaps_pte_range,
// 	.hugetlb_entry		= smaps_hugetlb_range,
// };

const struct mm_walk_ops smaps_shmem_walk_ops = {
	.pmd_entry		= smaps_pte_range,
	.hugetlb_entry		= smaps_hugetlb_range,
	.pte_hole		= smaps_pte_hole,
};

#endif /* CONFIG_PROC_PAGE_MONITOR */
