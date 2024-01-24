#ifndef HAGENT_PLACEMENT_MIGRATE_H
#define HAGENT_PLACEMENT_MIGRATE_H

#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <../../mm/internal.h>

#include "iheap.h"
#include "debug.h"

// reference:
//  - migrate_folio_unmap() in mm/migrate.c
//  - folio_get_anon_vma() in mm/rmap.c
//  - rmap_walk_anon() in mm/rmap.c
//  - migrate_to_node() in mm/mempolicy.c
//  - queue_folios_pte_range() in mm/mempolicy.c
static inline int migration_isolate_folios(struct indexable_heap *heap,
					   bool demotion,
					   struct list_head *isolated)
{
	int got = 0, filtered = 0, failed = 0,
	    candidate = indexable_heap_length(heap);
	while (got < migration_batch_size && indexable_heap_length(heap) > 0) {
		struct pair *p = indexable_heap_pop_back(heap);
		BUG_ON(!p);
		u64 pfn = p->key, count = p->value;

		if ((count > 1) ^ !demotion) {
			++filtered;
			continue;
		}
		// FIXME: make sure huge page is disabled
		struct folio *folio = pfn_folio(pfn);
		if (!folio || !folio_test_lru(folio) ||
		    !folio_isolate_lru(folio)) {
			++failed;
			continue;
		}
		// pr_info_ratelimited(
		// 	"%s: add pfn=0x%llx as migration candidate\n", __func__,
		// 	p->key);
		list_add_tail(&folio->lru, isolated);
		node_stat_mod_folio(folio,
				    NR_ISOLATED_ANON + folio_is_file_lru(folio),
				    folio_nr_pages(folio));
		++got;
	}
	if (got < migration_batch_size / 10 && !indexable_heap_length(heap)) {
		pr_info_ratelimited(
			"%s: not enough %s candidate candidate=%d got=%d filtered=%d failed=%d\n",
			__func__, demotion ? " demotion" : "promotion",
			candidate, got, filtered, failed);
	}

	return got;
}

static inline int migration_migrate_folios(struct list_head *isolated,
					   bool demotion)
{
	int target_nid = demotion ? PMEM_NID : DRAM_NID;
	nodemask_t target_mask = nodemask_of_node(target_nid);
	struct migration_target_control mtc = {
		.gfp_mask = (GFP_HIGHUSER_MOVABLE & ~__GFP_RECLAIM) |
			    __GFP_NOWARN | __GFP_NOMEMALLOC | GFP_NOWAIT,
		.nid = target_nid,
		.nmask = &target_mask,
	};
	// pr_info_ratelimited("%s: migrate_pages trying %lu pages\n", __func__,
	// 		    list_count_nodes(isolated));
	u32 succeeded = 0;
	int err = migrate_pages(isolated, alloc_migration_target, NULL,
				(unsigned long)&mtc, MIGRATE_ASYNC,
				MR_NUMA_MISPLACED, &succeeded);
	u64 failed = list_count_nodes(isolated);
	if (err) {
		putback_movable_pages(isolated);
	}
	// pr_info_ratelimited(
	// 	"%s: migrate_pages returned %d succeeded %u failed %llu\n",
	// 	__func__, err, succeeded, failed);

	return succeeded;
}

static inline int migration_do(struct indexable_heap *heap, bool demotion,
			       struct page_debug_info *page_debug_info)
{
	int to_nid = demotion ? PMEM_NID : DRAM_NID;
	struct zone *to_zone = NODE_DATA(to_nid)->node_zones + ZONE_NORMAL;

	u64 free = zone_page_state(to_zone, NR_FREE_PAGES),
	    wmark = wmark_pages(to_zone, WMARK_LOW);
	// pr_info_ratelimited("%s: free=%llu wmark=%llu\n",
	// 		    __func__, free, wmark);
	if (free <= wmark) {
		return -EAGAIN;
	}

	u64 isolated = {}, migrated = {}, ts = local_clock();
	u64 *pfns = {};
	struct list_head folios = LIST_HEAD_INIT(folios);
	struct folio *folio;
	scoped_guard(lru)
	{
		isolated = migration_isolate_folios(heap, demotion, &folios);
		if (!isolated) {
			// try again later
			return -EAGAIN;
		}
		pfns = kvcalloc(isolated, sizeof(u64), GFP_KERNEL);
		u64 i = 0;
		list_for_each_entry(folio, &folios, lru) {
			pfns[i++] = folio_pfn(folio);
		}
		pr_info_ratelimited("%s: %s isolated %llu pages\n", __func__,
				    demotion ? " demotion" : "promotion",
				    isolated);
		migrated = migration_migrate_folios(&folios, demotion);
		count_vm_events(demotion ? PAGE_DEMOTED : PAGE_PROMOTED,
				migrated);
		pr_info_ratelimited("%s: %s migrated %llu pages\n", __func__,
				    demotion ? " demotion" : "promotion",
				    migrated);
	}
	u64 last = 0;
	// Filter out those isolated but not migrated folios
	list_for_each_entry(folio, &folios, lru) {
		for (u64 i = 0; i < isolated; ++i) {
			u64 j = last + i % isolated;
			if (pfns[j] != folio_pfn(folio)) {
				continue;
			}
			pfns[last = j] = 0;
			break;
		}
	}
	if (debug_migration_latency) {
		// Only record the migration latency of those successfully
		// migrated pages
		for (u64 i = 0; i < isolated; ++i) {
			page_debug_info_mark_migrated(page_debug_info, pfns[i],
						      ts);
		}
	}
	// kvfree() will do nothing for a NULL pointer
	kvfree(pfns);
	return migrated;
}

#endif // !HAGENT_PLACEMENT_MIGRATE_H
