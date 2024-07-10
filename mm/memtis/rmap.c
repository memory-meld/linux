#include <linux/rmap.h>
#include <linux/random.h>
#include <linux/htmm.h>

struct htmm_cooling_arg {
	/*
     * page_is_hot: 0 --> already cooled.
     * page_is_hot: 1 --> cold after cooling
     * page_is_hot: 2 --> hot after cooling
     */
	int page_is_hot;
	struct mem_cgroup *memcg;
};

static bool cooling_page_one(struct page *page, struct vm_area_struct *vma,
			     unsigned long address, void *arg)
{
	struct htmm_cooling_arg *hca = arg;
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = address,
	};
	pginfo_t *pginfo;

	while (page_vma_mapped_walk(&pvmw)) {
		address = pvmw.address;
		page = pvmw.page;

		if (pvmw.pte) {
			struct page *pte_page;
			unsigned long prev_accessed, cur_idx;
			unsigned int memcg_cclock;
			pte_t *pte = pvmw.pte;

			pte_page = virt_to_page((unsigned long)pte);
			if (!PageHtmm(pte_page))
				continue;

			pginfo = get_pginfo_from_pte(pte);
			if (!pginfo)
				continue;

			spin_lock(&hca->memcg->access_lock);
			memcg_cclock = READ_ONCE(hca->memcg->cooling_clock);
			if (memcg_cclock > pginfo->cooling_clock) {
				unsigned int diff =
					memcg_cclock - pginfo->cooling_clock;
				int j;

				prev_accessed = pginfo->total_accesses;
				pginfo->nr_accesses = 0;
				for (j = 0; j < diff; j++)
					pginfo->total_accesses >>= 1;

				cur_idx = get_idx(pginfo->total_accesses);
				hca->memcg->hotness_hg[cur_idx]++;
				hca->memcg->ebp_hotness_hg[cur_idx]++;

				if (cur_idx >=
				    (hca->memcg->active_threshold - 1))
					hca->page_is_hot = 2;
				else
					hca->page_is_hot = 1;
				if (get_idx(prev_accessed) >=
				    (hca->memcg->bp_active_threshold))
					pginfo->may_hot = true;
				else
					pginfo->may_hot = false;
				pginfo->cooling_clock = memcg_cclock;
			}
			spin_unlock(&hca->memcg->access_lock);
		} else if (pvmw.pmd) {
			/* do nothing */
			continue;
		}
	}

	return true;
}

/**
 * cooling_page - cooling page and return true if the page is still hot page
 */
int cooling_page(struct page *page, struct mem_cgroup *memcg)
{
	struct htmm_cooling_arg hca = {
		.page_is_hot = 0,
		.memcg = memcg,
	};
	struct rmap_walk_control rwc = {
		.rmap_one = cooling_page_one,
		.arg = (void *)&hca,
	};

	if (!memcg || !memcg->htmm_enabled)
		return false;

	if (!PageAnon(page) || PageKsm(page))
		return false;

	if (!page_mapped(page))
		return false;

	rmap_walk(page, &rwc);
	return hca.page_is_hot;
}

static bool page_check_hotness_one(struct page *page,
				   struct vm_area_struct *vma,
				   unsigned long address, void *arg)
{
	struct htmm_cooling_arg *hca = arg;
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = address,
	};
	pginfo_t *pginfo;

	while (page_vma_mapped_walk(&pvmw)) {
		address = pvmw.address;
		page = pvmw.page;

		if (pvmw.pte) {
			struct page *pte_page;
			unsigned long cur_idx;
			pte_t *pte = pvmw.pte;

			pte_page = virt_to_page((unsigned long)pte);
			if (!PageHtmm(pte_page))
				continue;

			pginfo = get_pginfo_from_pte(pte);
			if (!pginfo)
				continue;

			cur_idx = pginfo->total_accesses;
			cur_idx = get_idx(cur_idx);
			if (cur_idx >= hca->memcg->active_threshold)
				hca->page_is_hot = 2;
			else
				hca->page_is_hot = 1;
		} else if (pvmw.pmd) {
			/* do nothing */
			continue;
		}
	}

	return true;
}

int page_check_hotness(struct page *page, struct mem_cgroup *memcg)
{
	struct htmm_cooling_arg hca = {
		.page_is_hot = 0,
		.memcg = memcg,
	};
	struct rmap_walk_control rwc = {
		.rmap_one = page_check_hotness_one,
		.arg = (void *)&hca,
	};

	if (!PageAnon(page) || PageKsm(page))
		return -1;

	if (!page_mapped(page))
		return -1;

	rmap_walk(page, &rwc);
	return hca.page_is_hot;
}

static bool get_pginfo_idx_one(struct page *page, struct vm_area_struct *vma,
			       unsigned long address, void *arg)
{
	struct htmm_cooling_arg *hca = arg;
	struct page_vma_mapped_walk pvmw = {
		.page = page,
		.vma = vma,
		.address = address,
	};
	pginfo_t *pginfo;

	while (page_vma_mapped_walk(&pvmw)) {
		address = pvmw.address;
		page = pvmw.page;

		if (pvmw.pte) {
			struct page *pte_page;
			unsigned long cur_idx;
			pte_t *pte = pvmw.pte;

			pte_page = virt_to_page((unsigned long)pte);
			if (!PageHtmm(pte_page))
				continue;

			pginfo = get_pginfo_from_pte(pte);
			if (!pginfo)
				continue;

			cur_idx = pginfo->total_accesses;
			cur_idx = get_idx(cur_idx);
			hca->page_is_hot = cur_idx;
		} else if (pvmw.pmd) {
			hca->page_is_hot = -1;
		}
	}

	return true;
}

int get_pginfo_idx(struct page *page)
{
	struct htmm_cooling_arg hca = {
		.page_is_hot = -1,
	};
	struct rmap_walk_control rwc = {
		.rmap_one = get_pginfo_idx_one,
		.arg = (void *)&hca,
	};

	if (!PageAnon(page) || PageKsm(page))
		return -1;

	if (!page_mapped(page))
		return -1;

	rmap_walk(page, &rwc);
	return hca.page_is_hot;
}
