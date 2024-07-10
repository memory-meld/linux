#include <linux/rmap.h>
#include <linux/migrate.h>
#include <linux/pagemap.h>
#include <linux/htmm.h>

// This function is deleted since 89f6c88 ("mm: __isolate_lru_page_prepare() in isolate_migratepages_block()")
/*
 * Attempt to remove the specified page from its LRU.  Only take this page
 * if it is of the appropriate PageActive status.  Pages which are being
 * freed elsewhere are also ignored.
 *
 * page:	page to consider
 * mode:	one of the LRU isolation modes defined above
 *
 * returns true on success, false on failure.
 */
bool __isolate_lru_page_prepare(struct page *page, isolate_mode_t mode)
{
	/* Only take pages on the LRU. */
	if (!PageLRU(page))
		return false;

	/* Compaction should not handle unevictable pages but CMA can do so */
	if (PageUnevictable(page) && !(mode & ISOLATE_UNEVICTABLE))
		return false;

	/*
	 * To minimise LRU disruption, the caller can indicate that it only
	 * wants to isolate pages it will be able to operate on without
	 * blocking - clean pages for the most part.
	 *
	 * ISOLATE_ASYNC_MIGRATE is used to indicate that it only wants to pages
	 * that it is possible to migrate without blocking
	 */
	if (mode & ISOLATE_ASYNC_MIGRATE) {
		/* All the caller can do on PageWriteback is block */
		if (PageWriteback(page))
			return false;

		if (PageDirty(page)) {
			struct address_space *mapping;
			bool migrate_dirty;

			/*
			 * Only pages without mappings or that have a
			 * ->migratepage callback are possible to migrate
			 * without blocking. However, we can be racing with
			 * truncation so it's necessary to lock the page
			 * to stabilise the mapping as truncation holds
			 * the page lock until after the page is removed
			 * from the page cache.
			 */
			if (!trylock_page(page))
				return false;

			mapping = page_mapping(page);
			migrate_dirty = !mapping || mapping->a_ops->migratepage;
			unlock_page(page);
			if (!migrate_dirty)
				return false;
		}
	}

	if ((mode & ISOLATE_UNMAPPED) && page_mapped(page))
		return false;

	return true;
}
bool try_to_unmap_clean(struct page_vma_mapped_walk *pvmw, struct page *page)
{
	void *addr;
	bool dirty;
	pte_t newpte;
	pginfo_t *pginfo;

	VM_BUG_ON_PAGE(PageCompound(page), page);
	VM_BUG_ON_PAGE(!PageAnon(page), page);
	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(pte_present(*pvmw->pte), page);

	if (PageMlocked(page) || (pvmw->vma->vm_flags & VM_LOCKED))
		return false;

	/* accessed ptes --> no zeroed pages */
	pginfo = get_pginfo_from_pte(pvmw->pte);
	if (!pginfo)
		return false;
	if (pginfo->nr_accesses > 0)
		return false;

	/*
	* The pmd entry mapping the old thp was flushed and the pte mapping
	* this subpage has been non present. Therefore, this subpage is
	* inaccessible. We don't need to remap it if it contains only zeros.
	*/
	addr = kmap_local_page(page);
	dirty = memchr_inv(addr, 0, PAGE_SIZE);
	kunmap_local(addr);

	if (dirty)
		return false;

	pte_clear_not_present_full(pvmw->vma->vm_mm, pvmw->address, pvmw->pte,
				   false);

	if (userfaultfd_armed(pvmw->vma)) {
		newpte = pte_mkspecial(
			pfn_pte(page_to_pfn(ZERO_PAGE(pvmw->address)),
				pvmw->vma->vm_page_prot));
		ptep_clear_flush(pvmw->vma, pvmw->address, pvmw->pte);
		set_pte_at(pvmw->vma->vm_mm, pvmw->address, pvmw->pte, newpte);
		dec_mm_counter(pvmw->vma->vm_mm, MM_ANONPAGES);
		return true;
	}

	dec_mm_counter(pvmw->vma->vm_mm, mm_counter(page));
	return true;
}
