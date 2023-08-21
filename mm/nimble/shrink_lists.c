#include "nimble.h"

static unsigned long shrink_lists_node_memcg(struct lruvec *lruvec,
					     struct scan_control *sc)
{
	unsigned long nr_reclaimed = 0;
	enum lru_list lru;

	for_each_evictable_lru(lru) {
		unsigned long nr_to_scan_local =
			lruvec_size_memcg_node(lru, sc->target_mem_cgroup,
					       lruvec_pgdat(lruvec)->node_id);
		nr_to_scan_local /= 2;
		/*nr_reclaimed += shrink_list(lru, nr_to_scan, lruvec, memcg, sc);*/
		/*
		 * for from(slow) node, we want active list, we start from the top of
		 * the active list. For pages in the bottom of
		 * the inactive list, we can place it to the top of inactive list
		 */
		/*
		 * for to(fast) node, we want inactive list, we start from the bottom of
		 * the inactive list. For pages in the active list, we just keep them.
		 */
		/*
		 * A key question is how many pages to scan each time, and what criteria
		 * to use to move pages between active/inactive page lists.
		 *  */
		if (is_active_lru(lru))
			nimble_shrink_active_list(nr_to_scan_local, lruvec, sc,
						  lru);
		else
			nr_reclaimed += nimble_shrink_inactive_list(
				nr_to_scan_local, lruvec, sc, lru);
	}
	cond_resched();

	return nr_reclaimed;
}

int shrink_lists(struct task_struct *p, struct mm_struct *mm,
			const nodemask_t *from, const nodemask_t *to,
			unsigned long nr_to_scan)
{
	struct mem_cgroup *memcg = mem_cgroup_from_task(p);
	// Fields that have ever been read inside `struct scan_control`
	// from kernel's `shrink_active_list()`:
	// - reclaim_idx
	// - may_unmap
	// - target_mem_cgroup
	// - priority
	// - order
	// priority, order are only used by tracing function, i.e.
	// - trace_mm_vmscan_lru_shrink_active
	// - trace_mm_vmscan_lru_isolate
	// `shrink_inactive_list` additionally checks:
	// - gfp_mask
	// - no_demotion
	// - proactive
	// And writes to:
	// - nr_scanned
	// - nr
	// Attention: proactive means the scanning is triggered from userspace
	// and demotion pass will be run (do_demotion_pass)
	struct scan_control sc = {
		.target_mem_cgroup = memcg,
		.may_unmap = 1,
		.reclaim_idx = MAX_NR_ZONES - 1,
	};
	// int from_nid, to_nid;
	struct lruvec *lruvec;
	VM_BUG_ON(!memcg);
	/* Let's handle simplest situation first */
	VM_BUG_ON(!(nodes_weight(*from) == 1 && nodes_weight(*to) == 1));

	if (memcg == root_mem_cgroup)
		return 0;

	lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(first_node(*from)));
	shrink_lists_node_memcg(lruvec, &sc);

	lruvec = mem_cgroup_lruvec(memcg, NODE_DATA(first_node(*to)));
	shrink_lists_node_memcg(lruvec, &sc);

	return 0;
}
// EXPORT_SYMBOL_GPL(shrink_lists);
