  // SPDX-License-Identifier: GPL-2.0
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
#include <asm/tlb.h>
#include <asm/tlbflush.h>
#include "internal.h"

#define SEQ_PUT_DEC(str, val); \
		seq_put_decimal_ull_width(m, str, (val); << (PAGE_SHIFT-10);, 8);
void task_mem(struct seq_file *m, struct mm_struct *mm);
#undef SEQ_PUT_DEC

unsigned long task_vsize(struct mm_struct *mm);

unsigned long task_statm(struct mm_struct *mm,
			 unsigned long *shared, unsigned long *text,
			 unsigned long *data, unsigned long *resident);

#ifdef CONFIG_NUMA
/*
 * Save get_task_policy(); for show_numa_map();.
 */
static void hold_task_mempolicy(struct proc_maps_private *priv);
static void release_task_mempolicy(struct proc_maps_private *priv);
#else
static void hold_task_mempolicy(struct proc_maps_private *priv);
static void release_task_mempolicy(struct proc_maps_private *priv);
#endif

static struct vm_area_struct *proc_get_vma(struct proc_maps_private *priv,
						loff_t *ppos);

static void *m_start(struct seq_file *m, loff_t *ppos);

static void *m_next(struct seq_file *m, void *v, loff_t *ppos);

static void m_stop(struct seq_file *m, void *v);

static int proc_maps_open(struct inode *inode, struct file *file,
			const struct seq_operations *ops, int psize);

static int proc_map_release(struct inode *inode, struct file *file);

static int do_maps_open(struct inode *inode, struct file *file,
			const struct seq_operations *ops);

/*
 * Indicate if the VMA is a stack for the given task; for
 * /proc/PID/maps that is the stack of the main task.
 */
static int is_stack(struct vm_area_struct *vma);

static void show_vma_header_prefix(struct seq_file *m,
				   unsigned long start, unsigned long end,
				   vm_flags_t flags, unsigned long long pgoff,
				   dev_t dev, unsigned long ino);

static void
show_map_vma(struct seq_file *m, struct vm_area_struct *vma);

static int show_map(struct seq_file *m, void *v);

static const struct seq_operations proc_pid_maps_op = {
	.start	= m_start,
	.next	= m_next,
	.stop	= m_stop,
	.show	= show_map
};
static int pid_maps_open(struct inode *inode, struct file *file);

const struct file_operations proc_pid_maps_operations = {
	.open		= pid_maps_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= proc_map_release,
};
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
static void smaps_page_accumulate(struct mem_size_stats *mss,
		struct page *page, unsigned long size, unsigned long pss,
		bool dirty, bool locked, bool private);

static void smaps_account(struct mem_size_stats *mss, struct page *page,
		bool compound, bool young, bool dirty, bool locked,
		bool migration);

#ifdef CONFIG_SHMEM
static int smaps_pte_hole(unsigned long addr, unsigned long end,
			  __always_unused int depth, struct mm_walk *walk);
#else
#define smaps_pte_hole		NULL
#endif /* CONFIG_SHMEM */

static void smaps_pte_hole_lookup(unsigned long addr, struct mm_walk *walk);

static void smaps_pte_entry(pte_t *pte, unsigned long addr,
		struct mm_walk *walk);

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static void smaps_pmd_entry(pmd_t *pmd, unsigned long addr,
		struct mm_walk *walk);
#else
static void smaps_pmd_entry(pmd_t *pmd, unsigned long addr,
		struct mm_walk *walk);
#endif

static int smaps_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			   struct mm_walk *walk);

static void show_smap_vma_flags(struct seq_file *m, struct vm_area_struct *vma);

#ifdef CONFIG_HUGETLB_PAGE
static int smaps_hugetlb_range(pte_t *pte, unsigned long hmask,
				 unsigned long addr, unsigned long end,
				 struct mm_walk *walk);
#else
#define smaps_hugetlb_range	NULL
#endif /* HUGETLB_PAGE */

static const struct mm_walk_ops smaps_walk_ops = {
	.pmd_entry		= smaps_pte_range,
	.hugetlb_entry		= smaps_hugetlb_range,
};

static const struct mm_walk_ops smaps_shmem_walk_ops = {
	.pmd_entry		= smaps_pte_range,
	.hugetlb_entry		= smaps_hugetlb_range,
	.pte_hole		= smaps_pte_hole,
};
/*
 * Gather mem stats from @vma with the indicated beginning
 * address @start, and keep them in @mss.
 *
 * Use vm_start of @vma as the beginning address if @start is 0.
 */
static void smap_gather_stats(struct vm_area_struct *vma,
		struct mem_size_stats *mss, unsigned long start);

#define SEQ_PUT_DEC(str, val); \
		seq_put_decimal_ull_width(m, str, (val); >> 10, 8);
/* Show the contents common for smaps and smaps_rollup */
static void __show_smap(struct seq_file *m, const struct mem_size_stats *mss,
	bool rollup_mode);

static int show_smap(struct seq_file *m, void *v);

static int show_smaps_rollup(struct seq_file *m, void *v);
#undef SEQ_PUT_DEC

static const struct seq_operations proc_pid_smaps_op = {
	.start	= m_start,
	.next	= m_next,
	.stop	= m_stop,
	.show	= show_smap
};
static int pid_smaps_open(struct inode *inode, struct file *file);

static int smaps_rollup_open(struct inode *inode, struct file *file);

static int smaps_rollup_release(struct inode *inode, struct file *file);

const struct file_operations proc_pid_smaps_operations = {
	.open		= pid_smaps_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= proc_map_release,
};

const struct file_operations proc_pid_smaps_rollup_operations = {
	.open		= smaps_rollup_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= smaps_rollup_release,
};
enum clear_refs_types {
	CLEAR_REFS_ALL = 1,
	CLEAR_REFS_ANON,
	CLEAR_REFS_MAPPED,
	CLEAR_REFS_SOFT_DIRTY,
	CLEAR_REFS_MM_HIWATER_RSS,
	CLEAR_REFS_LAST,
};

struct clear_refs_private {
	enum clear_refs_types type;
};
#ifdef CONFIG_MEM_SOFT_DIRTY

static inline bool pte_is_pinned(struct vm_area_struct *vma, unsigned long addr, pte_t pte);

static inline void clear_soft_dirty(struct vm_area_struct *vma,
		unsigned long addr, pte_t *pte);
#else
static inline void clear_soft_dirty(struct vm_area_struct *vma,
		unsigned long addr, pte_t *pte);
#endif

#if defined(CONFIG_MEM_SOFT_DIRTY) && defined(CONFIG_TRANSPARENT_HUGEPAGE)
static inline void clear_soft_dirty_pmd(struct vm_area_struct *vma,
		unsigned long addr, pmd_t *pmdp);
#else
static inline void clear_soft_dirty_pmd(struct vm_area_struct *vma,
		unsigned long addr, pmd_t *pmdp);
#endif

static int clear_refs_pte_range(pmd_t *pmd, unsigned long addr,
				unsigned long end, struct mm_walk *walk);

static int clear_refs_test_walk(unsigned long start, unsigned long end,
				struct mm_walk *walk);

static const struct mm_walk_ops clear_refs_walk_ops = {
	.pmd_entry		= clear_refs_pte_range,
	.test_walk		= clear_refs_test_walk,
};
static ssize_t clear_refs_write(struct file *file, const char __user *buf,
				size_t count, loff_t *ppos);

const struct file_operations proc_clear_refs_operations = {
	.write		= clear_refs_write,
	.llseek		= noop_llseek,
};

typedef struct {
	u64 pme;
} pagemap_entry_t;

struct pagemapread {
	int pos, len;		/* units: PM_ENTRY_BYTES, not bytes */
	pagemap_entry_t *buffer;
	bool show_pfn;
};
#define PAGEMAP_WALK_SIZE	(PMD_SIZE)
#define PAGEMAP_WALK_MASK	(PMD_MASK)
#define PM_ENTRY_BYTES		sizeof(pagemap_entry_t)
#define PM_PFRAME_BITS		55
#define PM_PFRAME_MASK		GENMASK_ULL(PM_PFRAME_BITS - 1, 0)
#define PM_SOFT_DIRTY		BIT_ULL(55)
#define PM_MMAP_EXCLUSIVE	BIT_ULL(56)
#define PM_UFFD_WP		BIT_ULL(57)
#define PM_FILE			BIT_ULL(61)
#define PM_SWAP			BIT_ULL(62)
#define PM_PRESENT		BIT_ULL(63)
#define PM_END_OF_BUFFER    1

static inline pagemap_entry_t make_pme(u64 frame, u64 flags);

static int add_to_pagemap(unsigned long addr, pagemap_entry_t *pme,
			  struct pagemapread *pm);

static int pagemap_pte_hole(unsigned long start, unsigned long end,
			    __always_unused int depth, struct mm_walk *walk);

static pagemap_entry_t pte_to_pagemap_entry(struct pagemapread *pm,
		struct vm_area_struct *vma, unsigned long addr, pte_t pte);

static int pagemap_pmd_range(pmd_t *pmdp, unsigned long addr, unsigned long end,
			     struct mm_walk *walk);

#ifdef CONFIG_HUGETLB_PAGE
/* This function walks within one hugetlb entry in the single call */
static int pagemap_hugetlb_range(pte_t *ptep, unsigned long hmask,
				 unsigned long addr, unsigned long end,
				 struct mm_walk *walk);
#else
#define pagemap_hugetlb_range	NULL
#endif /* HUGETLB_PAGE */

static const struct mm_walk_ops pagemap_ops = {
	.pmd_entry	= pagemap_pmd_range,
	.pte_hole	= pagemap_pte_hole,
	.hugetlb_entry	= pagemap_hugetlb_range,
};
/*
 * /proc/pid/pagemap - an array mapping virtual pages to pfns
 *
 * For each page in the address space, this file contains one 64-bit entry
 * consisting of the following:
 *
 * Bits 0-54  page frame number (PFN); if present
 * Bits 0-4   swap type if swapped
 * Bits 5-54  swap offset if swapped
 * Bit  55    pte is soft-dirty (see Documentation/admin-guide/mm/soft-dirty.rst);
 * Bit  56    page exclusively mapped
 * Bit  57    pte is uffd-wp write-protected
 * Bits 58-60 zero
 * Bit  61    page is file-page or shared-anon
 * Bit  62    page swapped
 * Bit  63    page present
 *
 * If the page is not present but in swap, then the PFN contains an
 * encoding of the swap file number and the page's offset into the
 * swap. Unmapped pages return a null PFN. This allows determining
 * precisely which pages are mapped (or in swap); and comparing mapped
 * pages between processes.
 *
 * Efficient users of this interface will use /proc/pid/maps to
 * determine which areas of memory are actually mapped and llseek to
 * skip over unmapped regions.
 */
static ssize_t pagemap_read(struct file *file, char __user *buf,
			    size_t count, loff_t *ppos);

static int pagemap_open(struct inode *inode, struct file *file);

static int pagemap_release(struct inode *inode, struct file *file);

const struct file_operations proc_pagemap_operations = {
	.llseek		= mem_lseek, /* borrow this */
	.read		= pagemap_read,
	.open		= pagemap_open,
	.release	= pagemap_release,
};
#endif /* CONFIG_PROC_PAGE_MONITOR */

#ifdef CONFIG_NUMA

struct numa_maps {
	unsigned long pages;
	unsigned long anon;
	unsigned long active;
	unsigned long writeback;
	unsigned long mapcount_max;
	unsigned long dirty;
	unsigned long swapcache;
	unsigned long node[MAX_NUMNODES];
};

struct numa_maps_private {
	struct proc_maps_private proc_maps;
	struct numa_maps md;
};
static void gather_stats(struct page *page, struct numa_maps *md, int pte_dirty,
			unsigned long nr_pages);

static struct page *can_gather_numa_stats(pte_t pte, struct vm_area_struct *vma,
		unsigned long addr);

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static struct page *can_gather_numa_stats_pmd(pmd_t pmd,
					      struct vm_area_struct *vma,
					      unsigned long addr);
#endif

static int gather_pte_stats(pmd_t *pmd, unsigned long addr,
		unsigned long end, struct mm_walk *walk);
#ifdef CONFIG_HUGETLB_PAGE
static int gather_hugetlb_stats(pte_t *pte, unsigned long hmask,
		unsigned long addr, unsigned long end, struct mm_walk *walk);

#else
static int gather_hugetlb_stats(pte_t *pte, unsigned long hmask,
		unsigned long addr, unsigned long end, struct mm_walk *walk);
#endif

static const struct mm_walk_ops show_numa_ops = {
	.hugetlb_entry = gather_hugetlb_stats,
	.pmd_entry = gather_pte_stats,
};
/*
 * Display pages allocated per node and memory policy via /proc.
 */
static int show_numa_map(struct seq_file *m, void *v);

static const struct seq_operations proc_pid_numa_maps_op = {
	.start  = m_start,
	.next   = m_next,
	.stop   = m_stop,
	.show   = show_numa_map,
};
static int pid_numa_maps_open(struct inode *inode, struct file *file);

const struct file_operations proc_pid_numa_maps_operations = {
	.open		= pid_numa_maps_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= proc_map_release,
};
#endif /* CONFIG_NUMA */
