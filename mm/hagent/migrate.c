#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>

// These are the additional exported symbols added
extern struct page *follow_page(struct vm_area_struct *vma,
				unsigned long address, unsigned int foll_flags);
extern int do_move_pages_to_node(struct mm_struct *mm,
				 struct list_head *pagelist, int node);
extern bool vma_migratable(struct vm_area_struct *vma);
extern bool isolate_hugetlb(struct folio *folio, struct list_head *list);
extern atomic_t lru_disable_count;
extern void lru_cache_disable(void);
extern bool isolate_lru_page(struct page *page);
extern struct mm_struct *find_mm_struct(pid_t pid, nodemask_t *mem_nodes);

static void do_pages_stat_array(struct mm_struct *mm, unsigned long nr_pages,
				u64 const *pages, int *status)
{
	unsigned long i;

	mmap_read_lock(mm);

	for (i = 0; i < nr_pages; i++) {
		unsigned long addr = (unsigned long)(*pages);
		struct vm_area_struct *vma;
		struct page *page;
		int err = -EFAULT;

		vma = vma_lookup(mm, addr);
		if (!vma)
			goto set_status;

		/* FOLL_DUMP to ignore special (like zero) pages */
		page = follow_page(vma, addr, FOLL_GET | FOLL_DUMP);

		err = PTR_ERR(page);
		if (IS_ERR(page))
			goto set_status;

		err = -ENOENT;
		if (!page)
			goto set_status;

		if (!is_zone_device_page(page))
			err = page_to_nid(page);

		put_page(page);
set_status:
		*status = err;

		pages++;
		status++;
	}

	mmap_read_unlock(mm);
}

static int do_pages_stat(struct mm_struct *mm, unsigned long nr_pages,
			 u64 const *pages, int *status)
{
#define DO_PAGES_STAT_CHUNK_NR 32UL
	u64 chunk_pages[DO_PAGES_STAT_CHUNK_NR];
	int chunk_status[DO_PAGES_STAT_CHUNK_NR];
	while (nr_pages) {
		unsigned long chunk_nr = min(nr_pages, DO_PAGES_STAT_CHUNK_NR);

		memcpy(chunk_pages, pages, chunk_nr * sizeof(*chunk_pages));
		do_pages_stat_array(mm, chunk_nr, chunk_pages, chunk_status);
		memcpy(status, chunk_status, chunk_nr * sizeof(*status));

		pages += chunk_nr;
		status += chunk_nr;
		nr_pages -= chunk_nr;
	}
	return nr_pages ? -EFAULT : 0;
}

static int store_status(int *status, int start, int value, int nr)
{
	while (nr-- > 0) {
		status[start] = value;
		start++;
	}
	return 0;
}

static int move_pages_and_store_status(struct mm_struct *mm, int node,
				       struct list_head *pagelist, int *status,
				       int start, int i, unsigned long nr_pages)
{
	int err;

	if (list_empty(pagelist))
		return 0;

	err = do_move_pages_to_node(mm, pagelist, node);
	if (err) {
		/*
		 * Positive err means the number of failed
		 * pages to migrate.  Since we are going to
		 * abort and return the number of non-migrated
		 * pages, so need to include the rest of the
		 * nr_pages that have not been attempted as
		 * well.
		 */
		if (err > 0)
			err += nr_pages - i;
		return err;
	}
	return store_status(status, start, node, i - start);
}

static int add_page_for_migration(struct mm_struct *mm, const u64 p, int node,
				  struct list_head *pagelist, bool migrate_all)
{
	struct vm_area_struct *vma;
	unsigned long addr;
	struct page *page;
	int err;
	bool isolated;

	mmap_read_lock(mm);
	addr = (unsigned long)untagged_addr_remote(mm, p);

	err = -EFAULT;
	vma = vma_lookup(mm, addr);
	if (!vma || !vma_migratable(vma))
		goto out;

	/* FOLL_DUMP to ignore special (like zero) pages */
	page = follow_page(vma, addr, FOLL_GET | FOLL_DUMP);

	err = PTR_ERR(page);
	if (IS_ERR(page))
		goto out;

	err = -ENOENT;
	if (!page)
		goto out;

	if (is_zone_device_page(page))
		goto out_putpage;

	err = 0;
	if (page_to_nid(page) == node)
		goto out_putpage;

	err = -EACCES;
	if (page_mapcount(page) > 1 && !migrate_all)
		goto out_putpage;

	if (PageHuge(page)) {
		if (PageHead(page)) {
			isolated = isolate_hugetlb(page_folio(page), pagelist);
			err = isolated ? 1 : -EBUSY;
		}
	} else {
		struct page *head;

		head = compound_head(page);
		isolated = isolate_lru_page(head);
		if (!isolated) {
			err = -EBUSY;
			goto out_putpage;
		}

		err = 1;
		list_add_tail(&head->lru, pagelist);
		mod_node_page_state(page_pgdat(head),
				    NR_ISOLATED_ANON + page_is_file_lru(head),
				    thp_nr_pages(head));
	}
out_putpage:
	/*
	 * Either remove the duplicate refcount from
	 * isolate_lru_page() or drop the page ref if it was
	 * not isolated.
	 */
	put_page(page);
out:
	mmap_read_unlock(mm);
	return err;
}

static int do_pages_move(struct mm_struct *mm, nodemask_t task_nodes,
			 unsigned long nr_pages, const u64 *pages,
			 const int *nodes, int *status, int flags)
{
	int current_node = NUMA_NO_NODE;
	LIST_HEAD(pagelist);
	int start, i;
	int err = 0, err1;

	lru_cache_disable();

	for (i = start = 0; i < nr_pages; i++) {
		u64 p;
		int node;

		err = -EFAULT;
		p = pages[i];
		node = nodes[i];

		err = -ENODEV;
		if (node < 0 || node >= MAX_NUMNODES)
			goto out_flush;
		if (!node_state(node, N_MEMORY))
			goto out_flush;

		err = -EACCES;
		if (!node_isset(node, task_nodes))
			goto out_flush;

		if (current_node == NUMA_NO_NODE) {
			current_node = node;
			start = i;
		} else if (node != current_node) {
			err = move_pages_and_store_status(mm, current_node,
							  &pagelist, status,
							  start, i, nr_pages);
			if (err)
				goto out;
			start = i;
			current_node = node;
		}

		/*
		 * Errors in the page lookup or isolation are not fatal and we simply
		 * report them via status
		 */
		err = add_page_for_migration(mm, p, current_node, &pagelist,
					     flags & MPOL_MF_MOVE_ALL);

		if (err > 0) {
			/* The page is successfully queued for migration */
			continue;
		}

		/*
		 * The move_pages() man page does not have an -EEXIST choice, so
		 * use -EFAULT instead.
		 */
		if (err == -EEXIST)
			err = -EFAULT;

		/*
		 * If the page is already on the target node (!err), store the
		 * node, otherwise, store the err.
		 */
		err = store_status(status, i, err ?: current_node, 1);
		if (err)
			goto out_flush;

		err = move_pages_and_store_status(mm, current_node, &pagelist,
						  status, start, i, nr_pages);
		if (err) {
			/* We have accounted for page i */
			if (err > 0)
				err--;
			goto out;
		}
		current_node = NUMA_NO_NODE;
	}
out_flush:
	/* Make sure we do not overwrite the existing error */
	err1 = move_pages_and_store_status(mm, current_node, &pagelist, status,
					   start, i, nr_pages);
	if (err >= 0)
		err = err1;
out:
	lru_cache_enable();
	return err;
}

int hagent_move_pages(pid_t pid, unsigned long nr_pages, const u64 *pages,
		      const int *nodes, int *status, int flags)
{
	struct mm_struct *mm;
	int err;
	nodemask_t task_nodes;

	/* Check flags */
	if (flags & ~(MPOL_MF_MOVE | MPOL_MF_MOVE_ALL))
		return -EINVAL;

	// if ((flags & MPOL_MF_MOVE_ALL) && !capable(CAP_SYS_NICE))
	// 	return -EPERM;

	mm = find_mm_struct(pid, &task_nodes);
	if (IS_ERR(mm))
		return PTR_ERR(mm);

	if (nodes)
		err = do_pages_move(mm, task_nodes, nr_pages, pages, nodes,
				    status, flags);
	else
		err = do_pages_stat(mm, nr_pages, pages, status);

	mmput(mm);
	return err;
}
