#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include <linux/migrate.h>
#include <linux/buffer_head.h>
#include <linux/page_owner.h>
#include <linux/sched/numa_balancing.h>
#include <linux/nomad.h>

#include <trace/events/migrate.h>

#include "../internal.h"

extern int move_to_new_page(struct page *newpage, struct page *page,
				enum migrate_mode mode);
extern int fallback_migrate_page(struct address_space *mapping,
	struct page *newpage, struct page *page, enum migrate_mode mode);
extern int unmap_and_move_huge_page(new_page_t get_new_page,
				free_page_t put_new_page, unsigned long private,
				struct page *hpage, int force,
				enum migrate_mode mode, int reason,
				struct list_head *ret);
extern inline int try_split_thp(struct page *page, struct page **page2,
				struct list_head *from);

static bool demotion_remove_migration_pte(struct page *page,
					  struct vm_area_struct *vma,
					  unsigned long addr, void *arg)
{
	struct demote_shadow_page_context *contxt =
		(struct demote_shadow_page_context *)arg;
	struct page_vma_mapped_walk pvmw = {
		.page = contxt->old_page,
		.vma = vma,
		.address = addr,
		.flags = PVMW_SYNC | PVMW_MIGRATION,
	};
	struct page *new;
	pte_t pte;
	swp_entry_t entry;

	VM_BUG_ON_PAGE(PageTail(page), page);
	while (page_vma_mapped_walk(&pvmw)) {
		if (PageKsm(page))
			new = page;
		else
			new = page - pvmw.page->index +
				linear_page_index(vma, pvmw.address);

#ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
		/* PMD-mapped THP migration entry */
		if (!pvmw.pte) {
			VM_BUG_ON_PAGE(PageHuge(page) || !PageTransCompound(page), page);
			remove_migration_pmd(&pvmw, new);
			continue;
		}
#endif

		get_page(new);
		pte = pte_mkold(mk_pte(new, READ_ONCE(vma->vm_page_prot)));
		if (pte_swp_soft_dirty(*pvmw.pte))
			pte = pte_mksoft_dirty(pte);

		/*
		 * Recheck VMA as permissions can change since migration started
		 */
		entry = pte_to_swp_entry(*pvmw.pte);
		if (is_writable_migration_entry(entry))
			pte = maybe_mkwrite(pte, vma);
		else if (pte_swp_uffd_wp(*pvmw.pte))
			pte = pte_mkuffd_wp(pte);

		if (unlikely(is_device_private_page(new))) {
			entry = make_readable_device_private_entry(swp_offset(entry));
			pte = swp_entry_to_pte(entry);
			if (pte_swp_soft_dirty(*pvmw.pte))
				pte = pte_swp_mksoft_dirty(pte);
			if (pte_swp_uffd_wp(*pvmw.pte))
				pte = pte_swp_mkuffd_wp(pte);
		}

#ifdef CONFIG_HUGETLB_PAGE
		if (PageHuge(new)) {
			unsigned int shift = huge_page_shift(hstate_vma(vma));
			pte = pte_mkhuge(pte);
			pte = arch_make_huge_pte(pte, shift, vma->vm_flags);
			set_huge_pte_at(vma->vm_mm, pvmw.address, pvmw.pte, pte);
			if (PageAnon(new))
				hugepage_add_anon_rmap(new, vma, pvmw.address);
			else
				page_dup_rmap(new, true);
		} else
#endif
		{
#ifdef CONFIG_NUMA_BALANCING
			if (page_is_demoted(page) && vma_migratable(vma)) {
				bool writable = pte_write(pte);

				pte = pte_modify(pte, PAGE_NONE);
				if (writable ||
				    contxt->was_writable_before_shadowed)
					pte = pte_mk_savedwrite(pte);
			}
#endif
			set_pte_at(vma->vm_mm, pvmw.address, pvmw.pte, pte);

			if (PageAnon(new))
				page_add_anon_rmap(new, vma, pvmw.address, false);
			else
				page_add_file_rmap(new, false);
		}
		if (vma->vm_flags & VM_LOCKED && !PageTransCompound(new))
			mlock_vma_page(new);

		if (PageTransHuge(page) && PageMlocked(page))
			clear_page_mlock(page);

		/* No need to invalidate - it was non-present before */
		update_mmu_cache(vma, pvmw.address, pvmw.pte);
	}

	return true;
}

void demotion_remove_migration_ptes(struct demote_shadow_page_context *contxt, struct page *new, bool locked)
{
	struct rmap_walk_control rwc = {
		.rmap_one = demotion_remove_migration_pte,
		.arg = contxt,
	};

	if (locked)
		rmap_walk_locked(new, &rwc);
	else
		rmap_walk(new, &rwc);
}


bool enable_copy_and_remap = true;

static void nomad_copy_page(struct page *page, struct page *newpage)
{
	BUG_ON(PageWriteback(page)); /* Writeback must be complete */

	migrate_page_copy(newpage, page);
}

// first round set the page table entries as clean, after that we move the pages
// finally check if the page table entries are dirty
static bool nomad_clean_pte(struct page *page, struct vm_area_struct *vma,
				 unsigned long addr, void *arg)
{

	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = addr,
		// we cannot have migrate flag, because rmap walker assert
		// the pte is a migration entry with migration flag while it's not
		.flags = PVMW_SYNC,
	};
	VM_BUG_ON_PAGE(PageTail(page), page);
	while (page_vma_mapped_walk(&pvmw)) {
		pte_t pte_val;
		// assert page tabe entry has value
		BUG_ON(!pvmw.pte);
		pte_val = *pvmw.pte;
		if (pte_dirty(pte_val)) {
			SetPageDirty(page);
			flush_cache_page(vma, addr, pte_pfn(*pvmw.pte));
			pte_val = ptep_clear_flush(vma, addr, pvmw.pte);
			// pte_val = pte_wrprotect(pte_val);
			pte_val = pte_mkclean(pte_val);
			set_pte_at(vma->vm_mm, addr, pvmw.pte, pte_val);
		}
	};
	return true;
}

static int page_not_mapped(struct page *page)
{
	return !page_mapped(page);
}

/**
 * @brief make preparations for the page duplication, we don't do
 * anything with the page table entry, but we need to lock the page
 * until all the duplication is over.
 * 
 * @param page 
 * @param newpage 
 * @param contxt 
 * @return int return 0 on success, non-zero value otherwise
 */
static int nomad_copy_and_remap_page(struct page *page,
					  struct page *newpage,
					  struct nomad_context *contxt)
{
	int ret = MIGRATEPAGE_SUCCESS;
	unsigned long old_flags;
	struct address_space *mapping;
	struct nomad_remap_status remap_arg = {
		.flags = TTU_IGNORE_MLOCK,
		.use_new_page = true,
		.new_page = newpage,
	};

	struct rmap_walk_control rwc = {
		.arg = NULL,
		.rmap_one = nomad_clean_pte,
		.done = page_not_mapped,
		.anon_lock = page_lock_anon_vma_read,
	};

	mapping = page_mapping(page);

	BUG_ON(mapping != NULL);

	// TODO(lingfeng): Now we only have one reference, we temporarily keep
	// the reference to the page and later modify that. Solve this for
	// shared pages.
	if (page_mapcount(page) != 1) {
		ret = -EAGAIN;
		goto out;
	};

	rmap_walk(page, &rwc);
	old_flags = READ_ONCE(page->flags);
	nomad_copy_page(page, newpage);
	// Since we do a copy and remapping, for an anonymous page, its ref count should be 2.
	// this page is still mapped at the moment.

	// TODO(lingfeng): metadata may be dirty during remapping, fix this later
	ret = migrate_page_move_mapping(mapping, newpage, page, 1);
	if (!ret) {
		bool unmapped_original_page =
			nomad_try_to_remap(page, &remap_arg);
		contxt->transactional_migrate_success_nr += 1;
		BUG_ON(unmapped_original_page != remap_arg.use_new_page);
	} else if (ret == -EAGAIN) {
		contxt->transactional_migrate_fail_nr += 1;
		WRITE_ONCE(page->flags, old_flags);
		goto out;
	} else {
		BUG();
	}

	if (remap_arg.use_new_page) {
		ret = MIGRATEPAGE_SUCCESS;
	} else {
		WRITE_ONCE(page->flags, old_flags);
		ret = -EAGAIN;
	}

out:
	return ret;
}

int nomad_migrate_page(struct address_space *mapping,
		struct page *newpage, struct page *page,
		enum migrate_mode mode)
{
	int rc;

	BUG_ON(PageWriteback(page));	/* Writeback must be complete */

	if (mode != MIGRATE_SYNC_NO_COPY)
		migrate_page_copy(newpage, page);
	else
		migrate_page_states(newpage, page);
	rc = migrate_page_move_mapping(mapping, newpage, page, 0);
	if (rc != MIGRATEPAGE_SUCCESS)
		return rc;

	return MIGRATEPAGE_SUCCESS;
}

int nomad_move_to_new_page(struct page *newpage, struct page *page,
				enum migrate_mode mode)
{
	struct address_space *mapping;
	int rc = -EAGAIN;
	bool is_lru = !__PageMovable(page);

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageLocked(newpage), newpage);

	mapping = page_mapping(page);

	if (likely(is_lru)) {
		if (!mapping)
			rc = nomad_migrate_page(mapping, newpage, page, mode);
		else if (mapping->a_ops->migratepage)
			/*
			 * Most pages have a mapping and most filesystems
			 * provide a migratepage callback. Anonymous pages
			 * are part of swap space which also has its own
			 * migratepage callback. This is the most common path
			 * for page migration.
			 */
			rc = mapping->a_ops->migratepage(mapping, newpage,
							page, mode);
		else
			rc = fallback_migrate_page(mapping, newpage,
							page, mode);
	} else {
		/*
		 * In case of non-lru page, it could be released after
		 * isolation step. In that case, we shouldn't try migration.
		 */
		VM_BUG_ON_PAGE(!PageIsolated(page), page);
		if (!PageMovable(page)) {
			rc = MIGRATEPAGE_SUCCESS;
			__ClearPageIsolated(page);
			goto out;
		}

		rc = mapping->a_ops->migratepage(mapping, newpage,
						page, mode);
		WARN_ON_ONCE(rc == MIGRATEPAGE_SUCCESS &&
			!PageIsolated(page));
	}

	/*
	 * When successful, old pagecache page->mapping must be cleared before
	 * page is freed; but stats require that PageAnon be left as PageAnon.
	 */
	if (rc == MIGRATEPAGE_SUCCESS) {
		if (__PageMovable(page)) {
			VM_BUG_ON_PAGE(!PageIsolated(page), page);

			/*
			 * We clear PG_movable under page_lock so any compactor
			 * cannot try to migrate this page.
			 */
			__ClearPageIsolated(page);
		}

		/*
		 * Anonymous and movable page->mapping will be cleared by
		 * free_pages_prepare so don't reset it here for keeping
		 * the type to work PageAnon, for example.
		 */
		if (!PageMappingFlags(page))
			page->mapping = NULL;

		if (likely(!is_zone_device_page(newpage)))
			flush_dcache_page(newpage);

	}
out:
	return rc;
}

static int __nomad_copy_and_remap(struct page *page, struct page *newpage,
				       int force, enum migrate_mode mode,
				       bool *go_cpy_remap,
				       struct nomad_context *contxt)
{
	int rc = -EAGAIN;
	int page_was_mapped = 0;
	struct anon_vma *anon_vma = NULL;
	bool is_lru = !__PageMovable(page);
	bool copy_and_remap = false;
	*go_cpy_remap = false;

	if (!trylock_page(page)) {
		if (!force || mode == MIGRATE_ASYNC)
			goto out;
		// we never expect this to happen
		BUG();
		/*
		 * It's not safe for direct compaction to call lock_page.
		 * For example, during page readahead pages are added locked
		 * to the LRU. Later, when the IO completes the pages are
		 * marked uptodate and unlocked. However, the queueing
		 * could be merging multiple pages for one bio (e.g.
		 * mpage_readahead). If an allocation happens for the
		 * second or third page, the process can end up locking
		 * the same page twice and deadlocking. Rather than
		 * trying to be clever about what pages can be locked,
		 * avoid the use of lock_page for direct compaction
		 * altogether.
		 */
		if (current->flags & PF_MEMALLOC)
			goto out;

		lock_page(page);
	}

	if (PageWriteback(page)) {
		/*
		 * Only in the case of a full synchronous migration is it
		 * necessary to wait for PageWriteback. In the async case,
		 * the retry loop is too short and in the sync-light case,
		 * the overhead of stalling is too much
		 */
		switch (mode) {
		case MIGRATE_SYNC:
		case MIGRATE_SYNC_NO_COPY:
			break;
		default:
			rc = -EBUSY;
			goto out_unlock;
		}
		if (!force)
			goto out_unlock;
		wait_on_page_writeback(page);
	}

	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 *
	 * Only page_get_anon_vma() understands the subtleties of
	 * getting a hold on an anon_vma from outside one of its mms.
	 * But if we cannot get anon_vma, then we won't need it anyway,
	 * because that implies that the anon page is no longer mapped
	 * (and cannot be remapped so long as we hold the page lock).
	 */
	if (PageAnon(page) && !PageKsm(page))
		anon_vma = page_get_anon_vma(page);

	/*
	 * Block others from accessing the new page when we get around to
	 * establishing additional references. We are usually the only one
	 * holding a reference to newpage at this point. We used to have a BUG
	 * here if trylock_page(newpage) fails, but would like to allow for
	 * cases where there might be a race with the previous use of newpage.
	 * This is much like races on refcount of oldpage: just don't BUG().
	 */
	if (unlikely(!trylock_page(newpage))) {
		// we never expect this to happen
		BUG();
		goto out_unlock;
	}

	if (unlikely(!is_lru)) {
		// we never expect this to happen
		BUG();
		rc = nomad_move_to_new_page(newpage, page, mode);
		goto out_unlock_both;
	}
	// TODO(lingfeng): only work on anonymous page that only has 2 reference
	// one comes from allocation, another comes from mapping
	copy_and_remap = (page_mapcount(page) == 1) && PageAnon(page) &&
			 enable_copy_and_remap;

	/*
	 * Corner case handling:
	 * 1. When a new swap-cache page is read into, it is added to the LRU
	 * and treated as swapcache but it has no rmap yet.
	 * Calling try_to_unmap() against a page->mapping==NULL page will
	 * trigger a BUG.  So handle it here.
	 * 2. An orphaned page (see truncate_cleanup_page) might have
	 * fs-private metadata. The page can be picked up due to memory
	 * offlining.  Everywhere else except page reclaim, the page is
	 * invisible to the vm, so the page can not be migrated.  So try to
	 * free the metadata, so the page can be freed.
	 */
	if (!page->mapping) {
		pr_info("[%s:%d], this is not expected", __FILE__, __LINE__);
		VM_BUG_ON_PAGE(PageAnon(page), page);
		// we never expect this to happen
		BUG();
		if (page_has_private(page)) {
			try_to_free_buffers(page);
			goto out_unlock_both;
		}
	} else if (page_mapped(page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(page) && !PageKsm(page) && !anon_vma,
			       page);
		if (copy_and_remap) {
			rc = nomad_copy_and_remap_page(page, newpage,
							    contxt);
		} else {
			try_to_unmap(page, 0);

			page_was_mapped = 1;
		}
	}

	if (copy_and_remap) {
		int tmp_err = 0;
		*go_cpy_remap = true;

		if (rc == MIGRATEPAGE_SUCCESS &&
		    async_mod_glob_ctrl.initialized &&
		    async_mod_glob_ctrl.link_shadow_page) {
			tmp_err = async_mod_glob_ctrl.link_shadow_page(newpage,
								       page);
		}

		BUG_ON(tmp_err);
	} else {
		if (!page_mapped(page))
			rc = move_to_new_page(newpage, page, mode);

		if (page_was_mapped)
			remove_migration_ptes(
				page,
				rc == MIGRATEPAGE_SUCCESS ? newpage : page,
				false, false);
	}

out_unlock_both:
	unlock_page(newpage);
out_unlock:
	/* Drop an anon_vma reference if we took one */
	if (anon_vma)
		put_anon_vma(anon_vma);
	unlock_page(page);
out:
	/*
	 * If migration is successful, decrease refcount of the newpage
	 * which will not free the page because new page owner increased
	 * refcounter. As well, if it is LRU page, add the page to LRU
	 * list in here. Use the old state of the isolated source page to
	 * determine if we migrated a LRU page. newpage was already unlocked
	 * and possibly modified by its owner - don't rely on the page
	 * state.
	 */
	if (copy_and_remap) {
		if (rc == MIGRATEPAGE_SUCCESS) {
			if (unlikely(!is_lru))
				put_page(newpage);
			else
				putback_lru_page(newpage);
		} else if (rc == -EAGAIN) {
		} else {
			BUG();
		}
	} else {
		// original execution in __unmap_and_move
		if (rc == MIGRATEPAGE_SUCCESS) {
			if (unlikely(!is_lru))
				put_page(newpage);
			else
				putback_lru_page(newpage);
		}
	}

	return rc;
}

static int nomad_unmap_and_move(new_page_t get_new_page,
				     free_page_t put_new_page,
				     unsigned long private, struct page *page,
				     int force, enum migrate_mode mode,
				     enum migrate_reason reason,
				     struct list_head *ret,
				     struct nomad_context *contxt)
{
	int rc = MIGRATEPAGE_SUCCESS;
	struct page *newpage = NULL;
	bool go_copy_and_remap;

	if (!thp_migration_supported() && PageTransHuge(page))
		return -ENOSYS;

	if (page_count(page) == 1) {
		/* page was freed from under us. So we are done. */
		ClearPageActive(page);
		ClearPageUnevictable(page);
		if (unlikely(__PageMovable(page))) {
			lock_page(page);
			if (!PageMovable(page))
				__ClearPageIsolated(page);
			unlock_page(page);
		}
		goto out;
	}

	newpage = get_new_page(page, private);
	if (!newpage)
		return -ENOMEM;

	/* TODO: check whether Ksm pages can be demoted? */
	if (reason == MR_DEMOTION && !PageKsm(page))
		set_page_demoted(newpage);

	rc = __nomad_copy_and_remap(page, newpage, force, mode,
					 &go_copy_and_remap, contxt);
	if (!go_copy_and_remap && rc == MIGRATEPAGE_SUCCESS)
		set_page_owner_migrate_reason(newpage, reason);
	if (!go_copy_and_remap) {
		goto out;
	}

	if (rc != -EAGAIN) {
		/*
		 * A page that has been migrated has all references
		 * removed and will be freed. A page that has not been
		 * migrated will have kept its references and be restored.
		 */
		list_del(&page->lru);
	}

	/*
	 * If migration is successful, releases reference grabbed during
	 * isolation. Otherwise, restore the page to right list unless
	 * we want to retry.
	 */
	if (rc == MIGRATEPAGE_SUCCESS) {
		/*
		 * Compaction can migrate also non-LRU pages which are
		 * not accounted to NR_ISOLATED_*. They can be recognized
		 * as __PageMovable
		 */
		if (likely(!__PageMovable(page)))
			mod_node_page_state(page_pgdat(page), NR_ISOLATED_ANON +
					page_is_file_lru(page), -thp_nr_pages(page));

		if (reason != MR_MEMORY_FAILURE) {
			/*
			 * We release the page in page_handle_poison.
			 */
			put_page(page);
		}

	} else {
		// Never expected this to happen
		BUG_ON(rc != -EAGAIN);

		// TODO(lingfeng): for shadowed page, we should not reclaim the new page
		if (put_new_page)
			put_new_page(newpage, private);
		else {
			put_page(newpage);
		}
	}

	return rc;

out:
	if (rc != -EAGAIN) {
		/*
		 * A page that has been migrated has all references
		 * removed and will be freed. A page that has not been
		 * migrated will have kept its references and be restored.
		 */
		list_del(&page->lru);
	}

		/*
		 * Compaction can migrate also non-LRU pages which are
		 * not accounted to NR_ISOLATED_*. They can be recognized
		 * as __PageMovable
		 */

	if (rc == MIGRATEPAGE_SUCCESS) {
		/*
		 * Compaction can migrate also non-LRU pages which are
		 * not accounted to NR_ISOLATED_*. They can be recognized
		 * as __PageMovable
		 */
		if (likely(!__PageMovable(page)))
			mod_node_page_state(page_pgdat(page),
					    NR_ISOLATED_ANON + page_is_file_lru(page),
					    -thp_nr_pages(page));

		if (reason != MR_MEMORY_FAILURE)
			/*
			 * We release the page in page_handle_poison.
			 */
			put_page(page);
	} else {
		if (rc != -EAGAIN)
			list_add_tail(&page->lru, ret);

		if (put_new_page)
			put_new_page(newpage, private);
		else
			put_page(newpage);
	}
	return rc;
}

int nomad_transit_pages(struct list_head *from, new_page_t get_new_page,
			     free_page_t put_new_page, unsigned long private,
			     enum migrate_mode mode, int reason,
			     unsigned int *nr_succeeded,
			     unsigned int *nr_retried, struct nomad_context *contxt)
{
	int retry = 1;
	int thp_retry = 1;
	int nr_failed = 0;
	int nr_thp_succeeded = 0;
	int nr_thp_failed = 0;
	int nr_thp_split = 0;
	int pass = 0;
	bool is_thp = false;
	struct page *page;
	struct page *page2;
	int swapwrite = current->flags & PF_SWAPWRITE;
	int rc, nr_subpages;
	LIST_HEAD(ret_pages);

	(*nr_retried) = 0;

	if (!swapwrite)
		current->flags |= PF_SWAPWRITE;

	for (pass = 0; pass < 10 && (retry || thp_retry); pass++) {
		retry = 0;
		thp_retry = 0;

		list_for_each_entry_safe(page, page2, from, lru) {
retry:
			/*
			 * THP statistics is based on the source huge page.
			 * Capture required information that might get lost
			 * during migration.
			 */
			is_thp = PageTransHuge(page) && !PageHuge(page);
			nr_subpages = thp_nr_pages(page);
			cond_resched();

			if (PageHuge(page))
				rc = unmap_and_move_huge_page(get_new_page,
						put_new_page, private, page,
						pass > 2, mode, reason,
						&ret_pages);
			else
				rc = nomad_unmap_and_move(
					get_new_page, put_new_page, private,
					page, pass > 2, mode, reason,
					&ret_pages, contxt);
			/*
			 * The rules are:
			 *	Success: non hugetlb page will be freed, hugetlb
			 *		 page will be put back
			 *	-EAGAIN: stay on the from list
			 *	-ENOMEM: stay on the from list
			 *	Other errno: put on ret_pages list then splice to
			 *		     from list
			 */
			switch(rc) {
			/*
			 * THP migration might be unsupported or the
			 * allocation could've failed so we should
			 * retry on the same page with the THP split
			 * to base pages.
			 *
			 * Head page is retried immediately and tail
			 * pages are added to the tail of the list so
			 * we encounter them after the rest of the list
			 * is processed.
			 */
			case -ENOSYS:
				/* THP migration is unsupported */
				if (is_thp) {
					if (!try_split_thp(page, &page2, from)) {
						nr_thp_split++;
						goto retry;
					}

					nr_thp_failed++;
					nr_failed += nr_subpages;
					break;
				}

				/* Hugetlb migration is unsupported */
				nr_failed++;
				break;
			case -ENOMEM:
				/*
				 * When memory is low, don't bother to try to migrate
				 * other pages, just exit.
				 */
				if (is_thp) {
					if (!try_split_thp(page, &page2, from)) {
						nr_thp_split++;
						goto retry;
					}

					nr_thp_failed++;
					nr_failed += nr_subpages;
					goto out;
				}
				nr_failed++;
				count_vm_events(PGMIGRATE_NOMEM_FAIL, thp_nr_pages(page));
				goto out;
			case -EAGAIN:
				if (is_thp) {
					thp_retry++;
					break;
				}
				(*nr_retried) += 1;
				retry++;
				break;
			case MIGRATEPAGE_SUCCESS:
				if (is_thp) {
					nr_thp_succeeded++;
					*nr_succeeded += nr_subpages;
					break;
				}
				(*nr_succeeded)++;
				break;
			default:
				/*
				 * Permanent failure (-EBUSY, etc.):
				 * unlike -EAGAIN case, the failed page is
				 * removed from migration page list and not
				 * retried in the next outer loop.
				 */
				if (is_thp) {
					nr_thp_failed++;
					nr_failed += nr_subpages;
					break;
				}
				nr_failed++;
				break;
			}
		}
	}
	nr_failed += retry + thp_retry;
	nr_thp_failed += thp_retry;
	rc = nr_failed;
out:
	/*
	 * Put the permanent failure page back to migration list, they
	 * will be put back to the right list by the caller.
	 */
	list_splice(&ret_pages, from);

	if (!swapwrite)
		current->flags &= ~PF_SWAPWRITE;

	return rc;
}
EXPORT_SYMBOL(nomad_transit_pages);

static int demotion_migrate_page(struct address_space *mapping,
				 struct page *newpage, struct page *page,
				 enum migrate_mode mode,
				 struct demote_shadow_page_context *contxt)
{
	int rc;
	int extra_count = 0;
	BUG_ON(PageWriteback(page)); /* Writeback must be complete */

	if (contxt->use_shadow_page) {
		// we inc the ref count when we find a shadow page
		extra_count = 1;
	}
	rc = migrate_page_move_mapping(mapping, newpage, page, extra_count);

	if (rc != MIGRATEPAGE_SUCCESS)
		return rc;

	if (mode != MIGRATE_SYNC_NO_COPY) {
		if (async_mod_glob_ctrl.initialized) {
			if (async_mod_glob_ctrl.demote_shadow_page_breakup) {
				bool use_shadow_page;
				// if new page is the "shadow page" of page, we directly skip
				// the copy
				use_shadow_page =
					async_mod_glob_ctrl
						.demote_shadow_page_breakup(
							page, contxt);
				if (use_shadow_page) {
					goto out;
				}
			}
		}
		migrate_page_copy(newpage, page);
	} else
		migrate_page_states(newpage, page);
out:
	return MIGRATEPAGE_SUCCESS;
}

static int demotion_move_to_new_page(struct page *newpage, struct page *page,
				     enum migrate_mode mode,
				     struct demote_shadow_page_context *contxt)
{
	struct address_space *mapping;
	int rc = -EAGAIN;
	bool is_lru = !__PageMovable(page);

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageLocked(newpage), newpage);

	mapping = page_mapping(page);

	if (likely(is_lru)) {
		if (!mapping)
			rc = demotion_migrate_page(mapping, newpage, page, mode,
						   contxt);
		else if (mapping->a_ops->migratepage)
			/*
			 * Most pages have a mapping and most filesystems
			 * provide a migratepage callback. Anonymous pages
			 * are part of swap space which also has its own
			 * migratepage callback. This is the most common path
			 * for page migration.
			 */
			rc = mapping->a_ops->migratepage(mapping, newpage, page,
							 mode);
		else
			rc = fallback_migrate_page(mapping, newpage, page,
						   mode);
	} else {
		/*
		 * In case of non-lru page, it could be released after
		 * isolation step. In that case, we shouldn't try migration.
		 */
		VM_BUG_ON_PAGE(!PageIsolated(page), page);
		if (!PageMovable(page)) {
			rc = MIGRATEPAGE_SUCCESS;
			__ClearPageIsolated(page);
			goto out;
		}

		rc = mapping->a_ops->migratepage(mapping, newpage, page, mode);
		WARN_ON_ONCE(rc == MIGRATEPAGE_SUCCESS && !PageIsolated(page));
	}

	/*
	 * When successful, old pagecache page->mapping must be cleared before
	 * page is freed; but stats require that PageAnon be left as PageAnon.
	 */
	if (rc == MIGRATEPAGE_SUCCESS) {
		if (__PageMovable(page)) {
			VM_BUG_ON_PAGE(!PageIsolated(page), page);

			/*
			 * We clear PG_movable under page_lock so any compactor
			 * cannot try to migrate this page.
			 */
			__ClearPageIsolated(page);
		}

		/*
		 * Anonymous and movable page->mapping will be cleared by
		 * free_pages_prepare so don't reset it here for keeping
		 * the type to work PageAnon, for example.
		 */
		if (!PageMappingFlags(page))
			page->mapping = NULL;

		if (likely(!is_zone_device_page(newpage)))
			flush_dcache_page(newpage);
	}
out:
	return rc;
}

static int __demotion_unmap_and_move(struct page *page, struct page *newpage, int force,
			    enum migrate_mode mode,
			    struct demote_shadow_page_context *contxt)
{
	int rc = -EAGAIN;
	int page_was_mapped = 0;
	struct anon_vma *anon_vma = NULL;
	bool is_lru = !__PageMovable(page);

	if (!trylock_page(page)) {
		if (!force || mode == MIGRATE_ASYNC)
			goto out;

		/*
		 * It's not safe for direct compaction to call lock_page.
		 * For example, during page readahead pages are added locked
		 * to the LRU. Later, when the IO completes the pages are
		 * marked uptodate and unlocked. However, the queueing
		 * could be merging multiple pages for one bio (e.g.
		 * mpage_readahead). If an allocation happens for the
		 * second or third page, the process can end up locking
		 * the same page twice and deadlocking. Rather than
		 * trying to be clever about what pages can be locked,
		 * avoid the use of lock_page for direct compaction
		 * altogether.
		 */
		if (current->flags & PF_MEMALLOC)
			goto out;

		lock_page(page);
	}

	if (PageWriteback(page)) {
		/*
		 * Only in the case of a full synchronous migration is it
		 * necessary to wait for PageWriteback. In the async case,
		 * the retry loop is too short and in the sync-light case,
		 * the overhead of stalling is too much
		 */
		switch (mode) {
		case MIGRATE_SYNC:
		case MIGRATE_SYNC_NO_COPY:
			break;
		default:
			rc = -EBUSY;
			goto out_unlock;
		}
		if (!force)
			goto out_unlock;
		wait_on_page_writeback(page);
	}

	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 *
	 * Only page_get_anon_vma() understands the subtleties of
	 * getting a hold on an anon_vma from outside one of its mms.
	 * But if we cannot get anon_vma, then we won't need it anyway,
	 * because that implies that the anon page is no longer mapped
	 * (and cannot be remapped so long as we hold the page lock).
	 */
	if (PageAnon(page) && !PageKsm(page))
		anon_vma = page_get_anon_vma(page);

	/*
	 * Block others from accessing the new page when we get around to
	 * establishing additional references. We are usually the only one
	 * holding a reference to newpage at this point. We used to have a BUG
	 * here if trylock_page(newpage) fails, but would like to allow for
	 * cases where there might be a race with the previous use of newpage.
	 * This is much like races on refcount of oldpage: just don't BUG().
	 */
	if (unlikely(!trylock_page(newpage)))
		goto out_unlock;

	if (unlikely(!is_lru)) {
		rc = demotion_move_to_new_page(newpage, page, mode, NULL);
		goto out_unlock_both;
	}

	/*
	 * Corner case handling:
	 * 1. When a new swap-cache page is read into, it is added to the LRU
	 * and treated as swapcache but it has no rmap yet.
	 * Calling try_to_unmap() against a page->mapping==NULL page will
	 * trigger a BUG.  So handle it here.
	 * 2. An orphaned page (see truncate_cleanup_page) might have
	 * fs-private metadata. The page can be picked up due to memory
	 * offlining.  Everywhere else except page reclaim, the page is
	 * invisible to the vm, so the page can not be migrated.  So try to
	 * free the metadata, so the page can be freed.
	 */
	if (!page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(page), page);
		if (page_has_private(page)) {
			try_to_free_buffers(page);
			goto out_unlock_both;
		}
	} else if (page_mapped(page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(page) && !PageKsm(page) && !anon_vma,
				page);
		contxt->traversed_mapping_num = 0;
		contxt->flags = (void *)(0);

		nomad_try_to_migrate(page, 0,
				      contxt);
		page_was_mapped = 1;
	}

	if (!page_mapped(page))
		rc = demotion_move_to_new_page(newpage, page, mode, contxt);

	if (page_was_mapped){
		contxt->old_page = page;
		demotion_remove_migration_ptes(contxt,
			rc == MIGRATEPAGE_SUCCESS ? newpage : page, false);
	}


out_unlock_both:
	unlock_page(newpage);
out_unlock:
	/* Drop an anon_vma reference if we took one */
	if (anon_vma)
		put_anon_vma(anon_vma);
	unlock_page(page);
out:
	/*
	 * If migration is successful, decrease refcount of the newpage
	 * which will not free the page because new page owner increased
	 * refcounter. As well, if it is LRU page, add the page to LRU
	 * list in here. Use the old state of the isolated source page to
	 * determine if we migrated a LRU page. newpage was already unlocked
	 * and possibly modified by its owner - don't rely on the page
	 * state.
	 */
	if (rc == MIGRATEPAGE_SUCCESS) {
		if (unlikely(!is_lru))
			put_page(newpage);
		else
			putback_lru_page(newpage);
	}

	return rc;
}

static int demotion_unmap_and_move(new_page_t get_new_page,
				   free_page_t put_new_page,
				   unsigned long private, struct page *page,
				   int force, enum migrate_mode mode,
				   enum migrate_reason reason,
				   struct list_head *ret)
{
	int rc = MIGRATEPAGE_SUCCESS;
	struct page *newpage = NULL;
	bool should_demote_page;
	struct demote_shadow_page_context contxt = {
		.use_shadow_page = 0,
		.shadow_page_ref_num = 0,
		.traversed_mapping_num = 0,
		.was_writable_before_shadowed = false,
		.made_writable = false,
		.shadow_page = NULL,
	};

	if (!thp_migration_supported() && PageTransHuge(page))
		return -ENOSYS;

	if (page_count(page) == 1) {
		/* page was freed from under us. So we are done. */
		ClearPageActive(page);
		ClearPageUnevictable(page);
		if (unlikely(__PageMovable(page))) {
			lock_page(page);
			if (!PageMovable(page))
				__ClearPageIsolated(page);
			unlock_page(page);
		}
		goto out;
	}

	if (reason == MR_DEMOTION && !PageKsm(page)) {
		should_demote_page = true;
	} else {
		should_demote_page = false;
	}

	if (should_demote_page && async_mod_glob_ctrl.initialized &&
	    async_mod_glob_ctrl.demote_shadow_page_find) {
		newpage = async_mod_glob_ctrl.demote_shadow_page_find(page,
								      &contxt);
		if (newpage) {
			goto use_shadowpage;
		}
	}

	newpage = get_new_page(page, private);
	if (!newpage)
		return -ENOMEM;
use_shadowpage:
	/* TODO: check whether Ksm pages can be demoted? */
	if (should_demote_page)
		set_page_demoted(newpage);

	rc = __demotion_unmap_and_move(page, newpage, force, mode, &contxt);

	if (likely(contxt.shadow_page_ref_num == 0)) {
	} else if (contxt.shadow_page_ref_num == 1) {
		// using a shadow page may fail, reclaim it
		put_page(contxt.shadow_page);
	} else {
		pr_err("error shadow_page_ref_num is %d, rc? %d, not expected",
		       contxt.shadow_page_ref_num, rc);
		BUG();
	}

	if (rc == MIGRATEPAGE_SUCCESS)
		set_page_owner_migrate_reason(newpage, reason);

out:
	if (rc != -EAGAIN) {
		/*
		 * A page that has been migrated has all references
		 * removed and will be freed. A page that has not been
		 * migrated will have kept its references and be restored.
		 */
		list_del(&page->lru);
	}

	/*
	 * If migration is successful, releases reference grabbed during
	 * isolation. Otherwise, restore the page to right list unless
	 * we want to retry.
	 */
	if (rc == MIGRATEPAGE_SUCCESS) {
		/*
		 * Compaction can migrate also non-LRU pages which are
		 * not accounted to NR_ISOLATED_*. They can be recognized
		 * as __PageMovable
		 */
		if (likely(!__PageMovable(page)))
			mod_node_page_state(page_pgdat(page), NR_ISOLATED_ANON +
					page_is_file_lru(page), -thp_nr_pages(page));

		if (reason != MR_MEMORY_FAILURE)
			/*
			 * We release the page in page_handle_poison.
			 */
			put_page(page);
	} else {
		if (rc != -EAGAIN)
			list_add_tail(&page->lru, ret);

		if (put_new_page)
			put_new_page(newpage, private);
		else
			put_page(newpage);
	}

	return rc;
}


int nomad_demotion_migrate_pages(struct list_head *from, new_page_t get_new_page,
		free_page_t put_new_page, unsigned long private,
		enum migrate_mode mode, int reason, unsigned int *nr_succeeded)
{
	int retry = 1;
	int thp_retry = 1;
	int nr_failed = 0;
	int nr_thp_succeeded = 0;
	int nr_thp_failed = 0;
	int nr_thp_split = 0;
	int pass = 0;
	bool is_thp = false;
	struct page *page;
	struct page *page2;
	int swapwrite = current->flags & PF_SWAPWRITE;
	int rc, nr_subpages;
	LIST_HEAD(ret_pages);

	trace_mm_migrate_pages_start(mode, reason);

	if (!swapwrite)
		current->flags |= PF_SWAPWRITE;

	for (pass = 0; pass < 10 && (retry || thp_retry); pass++) {
		retry = 0;
		thp_retry = 0;

		list_for_each_entry_safe(page, page2, from, lru) {
retry:
			/*
			 * THP statistics is based on the source huge page.
			 * Capture required information that might get lost
			 * during migration.
			 */
			is_thp = PageTransHuge(page) && !PageHuge(page);
			nr_subpages = thp_nr_pages(page);
			cond_resched();

			if (PageHuge(page))
				rc = unmap_and_move_huge_page(get_new_page,
						put_new_page, private, page,
						pass > 2, mode, reason,
						&ret_pages);
			else
				rc = demotion_unmap_and_move(get_new_page, put_new_page,
						private, page, pass > 2, mode,
						reason, &ret_pages);
			/*
			 * The rules are:
			 *	Success: non hugetlb page will be freed, hugetlb
			 *		 page will be put back
			 *	-EAGAIN: stay on the from list
			 *	-ENOMEM: stay on the from list
			 *	Other errno: put on ret_pages list then splice to
			 *		     from list
			 */
			switch(rc) {
			/*
			 * THP migration might be unsupported or the
			 * allocation could've failed so we should
			 * retry on the same page with the THP split
			 * to base pages.
			 *
			 * Head page is retried immediately and tail
			 * pages are added to the tail of the list so
			 * we encounter them after the rest of the list
			 * is processed.
			 */
			case -ENOSYS:
				/* THP migration is unsupported */
				if (is_thp) {
					if (!try_split_thp(page, &page2, from)) {
						nr_thp_split++;
						goto retry;
					}

					nr_thp_failed++;
					nr_failed += nr_subpages;
					break;
				}

				/* Hugetlb migration is unsupported */
				nr_failed++;
				break;
			case -ENOMEM:
				/*
				 * When memory is low, don't bother to try to migrate
				 * other pages, just exit.
				 */
				if (is_thp) {
					if (!try_split_thp(page, &page2, from)) {
						nr_thp_split++;
						goto retry;
					}

					nr_thp_failed++;
					nr_failed += nr_subpages;
					goto out;
				}
				nr_failed++;
				count_vm_events(PGMIGRATE_NOMEM_FAIL, thp_nr_pages(page));
				goto out;
			case -EAGAIN:
				if (is_thp) {
					thp_retry++;
					break;
				}
				retry++;
				break;
			case MIGRATEPAGE_SUCCESS:
				if (is_thp) {
					nr_thp_succeeded++;
					*nr_succeeded += nr_subpages;
					break;
				}
				(*nr_succeeded)++;
				break;
			default:
				/*
				 * Permanent failure (-EBUSY, etc.):
				 * unlike -EAGAIN case, the failed page is
				 * removed from migration page list and not
				 * retried in the next outer loop.
				 */
				if (is_thp) {
					nr_thp_failed++;
					nr_failed += nr_subpages;
					break;
				}
				nr_failed++;
				break;
			}
		}
	}
	nr_failed += retry + thp_retry;
	nr_thp_failed += thp_retry;
	rc = nr_failed;
out:
	/*
	 * Put the permanent failure page back to migration list, they
	 * will be put back to the right list by the caller.
	 */
	list_splice(&ret_pages, from);

	count_vm_events(PGMIGRATE_SUCCESS, *nr_succeeded);
	count_vm_events(PGMIGRATE_FAIL, nr_failed);
	count_vm_events(THP_MIGRATION_SUCCESS, nr_thp_succeeded);
	count_vm_events(THP_MIGRATION_FAIL, nr_thp_failed);
	count_vm_events(THP_MIGRATION_SPLIT, nr_thp_split);
	trace_mm_migrate_pages(*nr_succeeded, nr_failed, nr_thp_succeeded,
			       nr_thp_failed, nr_thp_split, mode, reason);

	if (!swapwrite)
		current->flags &= ~PF_SWAPWRITE;

	return rc;
}
