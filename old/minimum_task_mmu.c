#include "minimum_task_mmu.h"

#define SEQ_PUT_DEC(str, val) \
		seq_put_decimal_ull_width(m, str, (val) << (PAGE_SHIFT-10), 8)
#undef SEQ_PUT_DEC


/*
 * Proportional Set Size(PSS): my share of RSS.
 *
 * PSS of a process is the count of pages it has in memory, where each
 * page is divided by the number of processes sharing it.  So if a
 * process has 1000 pages all to itself, and 1000 shared with one other
 * process, its PSS will be 1500.
 *
 * To keep (accumulated) division errors low, we adopt a 64bit
 * fixed-point pss counter to minimize division errors. So (pss >>
 * PSS_SHIFT) would be the real byte count.
 *
 * A shift of 12 before division means (assuming 4K page size):
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
		bool dirty, bool locked, bool private)
{
	mss->pss += pss;

	if (PageAnon(page))
		mss->pss_anon += pss;
	else if (PageSwapBacked(page))
		mss->pss_shmem += pss;
	else
		mss->pss_file += pss;

	if (locked)
		mss->pss_locked += pss;

	if (dirty || PageDirty(page)) {
		mss->pss_dirty += pss;
		if (private)
			mss->private_dirty += size;
		else
			mss->shared_dirty += size;
	} else {
		if (private)
			mss->private_clean += size;
		else
			mss->shared_clean += size;
	}
}

void smaps_account(struct mem_size_stats *mss, struct page *page,
		bool compound, bool young, bool dirty, bool locked,
		bool migration)
{
	int i, nr = compound ? compound_nr(page) : 1;
	unsigned long size = nr * PAGE_SIZE;

	/*
	 * First accumulate quantities that depend only on |size| and the type
	 * of the compound page.
	 */
	if (PageAnon(page)) {
		mss->anonymous += size;
		if (!PageSwapBacked(page) && !dirty && !PageDirty(page))
			mss->lazyfree += size;
	}

	mss->resident += size;
	/* Accumulate the size in pages that have been accessed. */
	if (young || page_is_young(page) || PageReferenced(page))
		mss->referenced += size;

	/*
	 * Then accumulate quantities that may depend on sharing, or that may
	 * differ page-by-page.
	 *
	 * page_count(page) == 1 guarantees the page is mapped exactly once.
	 * If any subpage of the compound page mapped with PTE it would elevate
	 * page_count().
	 *
	 * The page_mapcount() is called to get a snapshot of the mapcount.
	 * Without holding the page lock this snapshot can be slightly wrong as
	 * we cannot always read the mapcount atomically.  It is not safe to
	 * call page_mapcount() even with PTL held if the page is not mapped,
	 * especially for migration entries.  Treat regular migration entries
	 * as mapcount == 1.
	 */
	if ((page_count(page) == 1) || migration) {
		smaps_page_accumulate(mss, page, size, size << PSS_SHIFT, dirty,
			locked, true);
		return;
	}
	for (i = 0; i < nr; i++, page++) {
		int mapcount = page_mapcount(page);
		unsigned long pss = PAGE_SIZE << PSS_SHIFT;
		if (mapcount >= 2)
			pss /= mapcount;
		smaps_page_accumulate(mss, page, PAGE_SIZE, pss, dirty, locked,
				      mapcount < 2);
	}
}

#ifdef CONFIG_SHMEM
int smaps_pte_hole(unsigned long addr, unsigned long end,
			  __always_unused int depth, struct mm_walk *walk)
{
	struct mem_size_stats *mss = walk->private;
	struct vm_area_struct *vma = walk->vma;

	mss->swap += shmem_partial_swap_usage(walk->vma->vm_file->f_mapping,
					      linear_page_index(vma, addr),
					      linear_page_index(vma, end));

	return 0;
}
#else
#define smaps_pte_hole		NULL
#endif /* CONFIG_SHMEM */

void smaps_pte_hole_lookup(unsigned long addr, struct mm_walk *walk)
{
#ifdef CONFIG_SHMEM
	if (walk->ops->pte_hole) {
		/* depth is not used */
		smaps_pte_hole(addr, addr + PAGE_SIZE, 0, walk);
	}
#endif
}

void smaps_pte_entry(pte_t *pte, unsigned long addr,
		struct mm_walk *walk)
{
	struct mem_size_stats *mss = walk->private;
	struct vm_area_struct *vma = walk->vma;
	bool locked = !!(vma->vm_flags & VM_LOCKED);
	struct page *page = NULL;
	bool migration = false, young = false, dirty = false;

	if (pte_present(*pte)) {
		page = vm_normal_page(vma, addr, *pte);
		young = pte_young(*pte);
		dirty = pte_dirty(*pte);
	} else if (is_swap_pte(*pte)) {
		swp_entry_t swpent = pte_to_swp_entry(*pte);

		if (!non_swap_entry(swpent)) {
			int mapcount;

			mss->swap += PAGE_SIZE;
			mapcount = swp_swapcount(swpent);
			if (mapcount >= 2) {
				u64 pss_delta = (u64)PAGE_SIZE << PSS_SHIFT;

				do_div(pss_delta, mapcount);
				mss->swap_pss += pss_delta;
			} else {
				mss->swap_pss += (u64)PAGE_SIZE << PSS_SHIFT;
			}
		} else if (is_pfn_swap_entry(swpent)) {
			if (is_migration_entry(swpent))
				migration = true;
			page = pfn_swap_entry_to_page(swpent);
		}
	} else {
		smaps_pte_hole_lookup(addr, walk);
		return;
	}

	if (!page)
		return;

	smaps_account(mss, page, false, young, dirty, locked, migration);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
void smaps_pmd_entry(pmd_t *pmd, unsigned long addr,
		struct mm_walk *walk)
{
	struct mem_size_stats *mss = walk->private;
	struct vm_area_struct *vma = walk->vma;
	bool locked = !!(vma->vm_flags & VM_LOCKED);
	struct page *page = NULL;
	bool migration = false;

	if (pmd_present(*pmd)) {
		/* FOLL_DUMP will return -EFAULT on huge zero page */
		page = follow_trans_huge_pmd(vma, addr, pmd, FOLL_DUMP);
	} else if (unlikely(thp_migration_supported() && is_swap_pmd(*pmd))) {
		swp_entry_t entry = pmd_to_swp_entry(*pmd);

		if (is_migration_entry(entry)) {
			migration = true;
			page = pfn_swap_entry_to_page(entry);
		}
	}
	if (IS_ERR_OR_NULL(page))
		return;
	if (PageAnon(page))
		mss->anonymous_thp += HPAGE_PMD_SIZE;
	else if (PageSwapBacked(page))
		mss->shmem_thp += HPAGE_PMD_SIZE;
	else if (is_zone_device_page(page))
		/* pass */;
	else
		mss->file_thp += HPAGE_PMD_SIZE;

	smaps_account(mss, page, true, pmd_young(*pmd), pmd_dirty(*pmd),
		      locked, migration);
}
#else
void smaps_pmd_entry(pmd_t *pmd, unsigned long addr,
		struct mm_walk *walk)
{
}
#endif

int smaps_pte_range(pmd_t *pmd, unsigned long addr, unsigned long end,
			   struct mm_walk *walk)
{
	struct vm_area_struct *vma = walk->vma;
	pte_t *pte;
	spinlock_t *ptl;

	ptl = pmd_trans_huge_lock(pmd, vma);
	if (ptl) {
		smaps_pmd_entry(pmd, addr, walk);
		spin_unlock(ptl);
		goto out;
	}

	if (pmd_trans_unstable(pmd))
		goto out;
	/*
	 * The mmap_lock held all the way back in m_start() is what
	 * keeps khugepaged out of here and from collapsing things
	 * in here.
	 */
	pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	for (; addr != end; pte++, addr += PAGE_SIZE)
		smaps_pte_entry(pte, addr, walk);
	pte_unmap_unlock(pte - 1, ptl);
out:
	cond_resched();
	return 0;
}

void show_smap_vma_flags(struct seq_file *m, struct vm_area_struct *vma)
{
	/*
	 * Don't forget to update Documentation/ on changes.
	 */
	static const char mnemonics[BITS_PER_LONG][2] = {
		/*
		 * In case if we meet a flag we don't know about.
		 */
		[0 ... (BITS_PER_LONG-1)] = "??",

		[ilog2(VM_READ)]	= "rd",
		[ilog2(VM_WRITE)]	= "wr",
		[ilog2(VM_EXEC)]	= "ex",
		[ilog2(VM_SHARED)]	= "sh",
		[ilog2(VM_MAYREAD)]	= "mr",
		[ilog2(VM_MAYWRITE)]	= "mw",
		[ilog2(VM_MAYEXEC)]	= "me",
		[ilog2(VM_MAYSHARE)]	= "ms",
		[ilog2(VM_GROWSDOWN)]	= "gd",
		[ilog2(VM_PFNMAP)]	= "pf",
		[ilog2(VM_LOCKED)]	= "lo",
		[ilog2(VM_IO)]		= "io",
		[ilog2(VM_SEQ_READ)]	= "sr",
		[ilog2(VM_RAND_READ)]	= "rr",
		[ilog2(VM_DONTCOPY)]	= "dc",
		[ilog2(VM_DONTEXPAND)]	= "de",
		[ilog2(VM_LOCKONFAULT)]	= "lf",
		[ilog2(VM_ACCOUNT)]	= "ac",
		[ilog2(VM_NORESERVE)]	= "nr",
		[ilog2(VM_HUGETLB)]	= "ht",
		[ilog2(VM_SYNC)]	= "sf",
		[ilog2(VM_ARCH_1)]	= "ar",
		[ilog2(VM_WIPEONFORK)]	= "wf",
		[ilog2(VM_DONTDUMP)]	= "dd",
#ifdef CONFIG_ARM64_BTI
		[ilog2(VM_ARM64_BTI)]	= "bt",
#endif
#ifdef CONFIG_MEM_SOFT_DIRTY
		[ilog2(VM_SOFTDIRTY)]	= "sd",
#endif
		[ilog2(VM_MIXEDMAP)]	= "mm",
		[ilog2(VM_HUGEPAGE)]	= "hg",
		[ilog2(VM_NOHUGEPAGE)]	= "nh",
		[ilog2(VM_MERGEABLE)]	= "mg",
		[ilog2(VM_UFFD_MISSING)]= "um",
		[ilog2(VM_UFFD_WP)]	= "uw",
#ifdef CONFIG_ARM64_MTE
		[ilog2(VM_MTE)]		= "mt",
		[ilog2(VM_MTE_ALLOWED)]	= "",
#endif
#ifdef CONFIG_ARCH_HAS_PKEYS
		/* These come out via ProtectionKey: */
		[ilog2(VM_PKEY_BIT0)]	= "",
		[ilog2(VM_PKEY_BIT1)]	= "",
		[ilog2(VM_PKEY_BIT2)]	= "",
		[ilog2(VM_PKEY_BIT3)]	= "",
#if VM_PKEY_BIT4
		[ilog2(VM_PKEY_BIT4)]	= "",
#endif
#endif /* CONFIG_ARCH_HAS_PKEYS */
#ifdef CONFIG_HAVE_ARCH_USERFAULTFD_MINOR
		[ilog2(VM_UFFD_MINOR)]	= "ui",
#endif /* CONFIG_HAVE_ARCH_USERFAULTFD_MINOR */
	};
	size_t i;

	seq_puts(m, "VmFlags: ");
	for (i = 0; i < BITS_PER_LONG; i++) {
		if (!mnemonics[i][0])
			continue;
		if (vma->vm_flags & (1UL << i)) {
			seq_putc(m, mnemonics[i][0]);
			seq_putc(m, mnemonics[i][1]);
			seq_putc(m, ' ');
		}
	}
	seq_putc(m, '\n');
}

#ifdef CONFIG_HUGETLB_PAGE
int smaps_hugetlb_range(pte_t *pte, unsigned long hmask,
				 unsigned long addr, unsigned long end,
				 struct mm_walk *walk)
{
	struct mem_size_stats *mss = walk->private;
	struct vm_area_struct *vma = walk->vma;
	struct page *page = NULL;

	if (pte_present(*pte)) {
		page = vm_normal_page(vma, addr, *pte);
	} else if (is_swap_pte(*pte)) {
		swp_entry_t swpent = pte_to_swp_entry(*pte);

		if (is_pfn_swap_entry(swpent))
			page = pfn_swap_entry_to_page(swpent);
	}
	if (page) {
		if (page_mapcount(page) >= 2 || hugetlb_pmd_shared(pte))
			mss->shared_hugetlb += huge_page_size(hstate_vma(vma));
		else
			mss->private_hugetlb += huge_page_size(hstate_vma(vma));
	}
	return 0;
}
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
