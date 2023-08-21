#include "nimble.h"

extern bool can_demote(int nid, struct scan_control *sc);

extern unsigned int move_folios_to_lru(struct lruvec *lruvec,
		struct list_head *list);

extern unsigned long isolate_lru_folios(unsigned long nr_to_scan,
		struct lruvec *lruvec, struct list_head *dst,
		unsigned long *nr_scanned, struct scan_control *sc,
		enum lru_list lru);

extern enum folio_references folio_check_references(struct folio *folio,
						  struct scan_control *sc);

static unsigned int nimble_shrink_folio_list(struct list_head *folio_list,
		struct pglist_data *pgdat, struct scan_control *sc,
		struct reclaim_stat *stat, bool ignore_references)
{
	LIST_HEAD(ret_folios);
	LIST_HEAD(free_folios);
	LIST_HEAD(demote_folios);
	unsigned int nr_reclaimed = 0;
	// unsigned int pgactivate = 0;
	bool do_demote_pass;
	// struct swap_iocb *plug = NULL;

	memset(stat, 0, sizeof(*stat));
	cond_resched();
	do_demote_pass = can_demote(pgdat->node_id, sc);

// retry:
	while (!list_empty(folio_list)) {
		// struct address_space *mapping;
		struct folio *folio;
		enum folio_references references = FOLIOREF_RECLAIM;
		// bool dirty, writeback;
		unsigned int nr_pages;

		cond_resched();

		folio = lru_to_folio(folio_list);
		list_del(&folio->lru);

		if (!folio_trylock(folio))
			goto keep;

		VM_BUG_ON_FOLIO(folio_test_active(folio), folio);

		nr_pages = folio_nr_pages(folio);

		/* Account the number of base pages */
		// sc->nr_scanned += nr_pages;

		if (unlikely(!folio_evictable(folio)))
			goto activate_locked;

		if (!sc->may_unmap && folio_mapped(folio))
			goto keep_locked;

		if (!ignore_references)
			references = folio_check_references(folio, sc);

		switch (references) {
		case FOLIOREF_ACTIVATE:
			goto activate_locked;
		// case FOLIOREF_KEEP:
		// 	stat->nr_ref_keep += nr_pages;
		// 	goto keep_locked;
		// case FOLIOREF_RECLAIM:
		// case FOLIOREF_RECLAIM_CLEAN:
		// 	; /* try to reclaim the folio below */
		default:
			stat->nr_ref_keep += nr_pages;
			goto keep_locked;
		}

		/*
		 * Before reclaiming the folio, try to relocate
		 * its contents to another node.
		 */
		// if (do_demote_pass &&
		//     (thp_migration_supported() || !folio_test_large(folio))) {
		// 	list_add(&folio->lru, &demote_folios);
		// 	folio_unlock(folio);
		// 	continue;
		// }

		// relcaim/swap logic deleted


activate_locked:
		/* Not a candidate for swapping, so reclaim swap space. */
		if (folio_test_swapcache(folio) &&
		    (mem_cgroup_swap_full(folio) || folio_test_mlocked(folio)))
			folio_free_swap(folio);
		VM_BUG_ON_FOLIO(folio_test_active(folio), folio);
		if (!folio_test_mlocked(folio)) {
			int type = folio_is_file_lru(folio);
			folio_set_active(folio);
			stat->nr_activate[type] += nr_pages;
			// count_memcg_folio_events(folio, PGACTIVATE, nr_pages);
		}
keep_locked:
		folio_unlock(folio);
keep:
		list_add(&folio->lru, &ret_folios);
		VM_BUG_ON_FOLIO(folio_test_lru(folio) ||
				folio_test_unevictable(folio), folio);
	}
	/* 'folio_list' is always empty here */

	/* Migrate folios selected for demotion */
	// nr_reclaimed += demote_folio_list(&demote_folios, pgdat);
	/* Folios that could not be demoted are still in @demote_folios */
	// if (!list_empty(&demote_folios)) {
	// 	/* Folios which weren't demoted go back on @folio_list */
	// 	list_splice_init(&demote_folios, folio_list);

	// 	/*
	// 	 * goto retry to reclaim the undemoted folios in folio_list if
	// 	 * desired.
	// 	 *
	// 	 * Reclaiming directly from top tier nodes is not often desired
	// 	 * due to it breaking the LRU ordering: in general memory
	// 	 * should be reclaimed from lower tier nodes and demoted from
	// 	 * top tier nodes.
	// 	 *
	// 	 * However, disabling reclaim from top tier nodes entirely
	// 	 * would cause ooms in edge scenarios where lower tier memory
	// 	 * is unreclaimable for whatever reason, eg memory being
	// 	 * mlocked or too hot to reclaim. We can disable reclaim
	// 	 * from top tier nodes in proactive reclaim though as that is
	// 	 * not real memory pressure.
	// 	 */
	// 	if (!sc->proactive) {
	// 		do_demote_pass = false;
	// 		goto retry;
	// 	}
	// }

	// pgactivate = stat->nr_activate[0] + stat->nr_activate[1];

	mem_cgroup_uncharge_list(&free_folios);
	try_to_unmap_flush();
	free_unref_page_list(&free_folios);

	list_splice(&ret_folios, folio_list);
	// count_vm_events(PGACTIVATE, pgactivate);

	// if (plug)
	// 	swap_write_unplug(plug);
	return nr_reclaimed;
}

unsigned long nimble_shrink_inactive_list(unsigned long nr_to_scan,
		struct lruvec *lruvec, struct scan_control *sc,
		enum lru_list lru)
{
	LIST_HEAD(folio_list);
	unsigned long nr_scanned;
	unsigned int nr_reclaimed = 0;
	unsigned long nr_taken;
	struct reclaim_stat stat;
	bool file = is_file_lru(lru);
	// enum vm_event_item item;
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	// bool stalled = false;

	// while (unlikely(too_many_isolated(pgdat, file, sc))) {
	// 	if (stalled)
	// 		return 0;

	// 	/* wait a bit for the reclaimer. */
	// 	stalled = true;
	// 	reclaim_throttle(pgdat, VMSCAN_THROTTLE_ISOLATED);

	// 	/* We are about to die and free our memory. Return now. */
	// 	if (fatal_signal_pending(current))
	// 		return SWAP_CLUSTER_MAX;
	// }

	lru_add_drain();

	spin_lock_irq(&lruvec->lru_lock);

	nr_taken = isolate_lru_folios(nr_to_scan, lruvec, &folio_list,
				     &nr_scanned, sc, lru);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);
	// item = PGSCAN_KSWAPD + reclaimer_offset();
	// if (!cgroup_reclaim(sc))
	// 	__count_vm_events(item, nr_scanned);
	// __count_memcg_events(lruvec_memcg(lruvec), item, nr_scanned);
	// __count_vm_events(PGSCAN_ANON + file, nr_scanned);

	spin_unlock_irq(&lruvec->lru_lock);

	if (nr_taken == 0)
		return 0;

	nr_reclaimed = nimble_shrink_folio_list(&folio_list, pgdat, sc, &stat, false);

	spin_lock_irq(&lruvec->lru_lock);
	move_folios_to_lru(lruvec, &folio_list);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
	// item = PGSTEAL_KSWAPD + reclaimer_offset();
	// if (!cgroup_reclaim(sc))
	// 	__count_vm_events(item, nr_reclaimed);
	// __count_memcg_events(lruvec_memcg(lruvec), item, nr_reclaimed);
	// __count_vm_events(PGSTEAL_ANON + file, nr_reclaimed);
	spin_unlock_irq(&lruvec->lru_lock);

	// lru_note_cost(lruvec, file, stat.nr_pageout, nr_scanned - nr_reclaimed);
	mem_cgroup_uncharge_list(&folio_list);
	free_unref_page_list(&folio_list);

	/*
	 * If dirty folios are scanned that are not queued for IO, it
	 * implies that flushers are not doing their job. This can
	 * happen when memory pressure pushes dirty folios to the end of
	 * the LRU before the dirty limits are breached and the dirty
	 * data has expired. It can also happen when the proportion of
	 * dirty folios grows not through writes but through memory
	 * pressure reclaiming all the clean cache. And in some cases,
	 * the flushers simply cannot keep up with the allocation
	 * rate. Nudge the flusher threads in case they are asleep.
	 */
	// if (stat.nr_unqueued_dirty == nr_taken) {
	// 	wakeup_flusher_threads(WB_REASON_VMSCAN);
	// 	/*
	// 	 * For cgroupv1 dirty throttling is achieved by waking up
	// 	 * the kernel flusher here and later waiting on folios
	// 	 * which are in writeback to finish (see shrink_folio_list()).
	// 	 *
	// 	 * Flusher may not be able to issue writeback quickly
	// 	 * enough for cgroupv1 writeback throttling to work
	// 	 * on a large system.
	// 	 */
	// 	if (!writeback_throttling_sane(sc))
	// 		reclaim_throttle(pgdat, VMSCAN_THROTTLE_WRITEBACK);
	// }

	// sc->nr.dirty += stat.nr_dirty;
	// sc->nr.congested += stat.nr_congested;
	// sc->nr.unqueued_dirty += stat.nr_unqueued_dirty;
	// sc->nr.writeback += stat.nr_writeback;
	// sc->nr.immediate += stat.nr_immediate;
	// sc->nr.taken += nr_taken;
	// if (file)
	// 	sc->nr.file_taken += nr_taken;

	// trace_mm_vmscan_lru_shrink_inactive(pgdat->node_id,
	// 		nr_scanned, nr_reclaimed, &stat, sc->priority, file);
	return nr_reclaimed;
}

void nimble_shrink_active_list(unsigned long nr_to_scan,
			       struct lruvec *lruvec,
			       struct scan_control *sc,
			       enum lru_list lru)
{
	unsigned long nr_taken;
	unsigned long nr_scanned;
	unsigned long vm_flags;
	LIST_HEAD(l_hold);	/* The folios which were snipped off */
	LIST_HEAD(l_active);
	LIST_HEAD(l_inactive);
	unsigned nr_deactivate, nr_activate;
	unsigned nr_rotated = 0;
	int file = is_file_lru(lru);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	lru_add_drain();

	spin_lock_irq(&lruvec->lru_lock);

	nr_taken = isolate_lru_folios(nr_to_scan, lruvec, &l_hold,
				     &nr_scanned, sc, lru);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);

	// if (!cgroup_reclaim(sc))
	// 	__count_vm_events(PGREFILL, nr_scanned);
	// __count_memcg_events(lruvec_memcg(lruvec), PGREFILL, nr_scanned);

	spin_unlock_irq(&lruvec->lru_lock);

	while (!list_empty(&l_hold)) {
		struct folio *folio;

		cond_resched();
		folio = lru_to_folio(&l_hold);
		list_del(&folio->lru);

		if (unlikely(!folio_evictable(folio))) {
			folio_putback_lru(folio);
			continue;
		}

		// if (unlikely(buffer_heads_over_limit)) {
		// 	if (folio_test_private(folio) && folio_trylock(folio)) {
		// 		if (folio_test_private(folio))
		// 			filemap_release_folio(folio, 0);
		// 		folio_unlock(folio);
		// 	}
		// }

		/* Referenced or rmap lock contention: rotate */
		if (folio_referenced(folio, 0, sc->target_mem_cgroup,
				     &vm_flags) != 0) {
			/*
			 * Identify referenced, file-backed active folios and
			 * give them one more trip around the active list. So
			 * that executable code get better chances to stay in
			 * memory under moderate memory pressure.  Anon folios
			 * are not likely to be evicted by use-once streaming
			 * IO, plus JVM can create lots of anon VM_EXEC folios,
			 * so we ignore them here.
			 */
			if ((vm_flags & VM_EXEC) && folio_is_file_lru(folio)) {
				nr_rotated += folio_nr_pages(folio);
				list_add(&folio->lru, &l_active);
				continue;
			}
		}

		folio_clear_active(folio);	/* we are de-activating */
		// folio_set_workingset(folio);
		list_add(&folio->lru, &l_inactive);
	}

	/*
	 * Move folios back to the lru list.
	 */
	spin_lock_irq(&lruvec->lru_lock);

	nr_activate = move_folios_to_lru(lruvec, &l_active);
	nr_deactivate = move_folios_to_lru(lruvec, &l_inactive);
	/* Keep all free folios in l_active list */
	list_splice(&l_inactive, &l_active);

	// __count_vm_events(PGDEACTIVATE, nr_deactivate);
	// __count_memcg_events(lruvec_memcg(lruvec), PGDEACTIVATE, nr_deactivate);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
	spin_unlock_irq(&lruvec->lru_lock);

	// if (nr_rotated)
	// 	lru_note_cost(lruvec, file, 0, nr_rotated);
	mem_cgroup_uncharge_list(&l_active);
	free_unref_page_list(&l_active);
	// trace_mm_vmscan_lru_shrink_active(pgdat->node_id, nr_taken, nr_activate,
	// 		nr_deactivate, nr_rotated, sc->priority, file);
}
