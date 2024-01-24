#include "placement.h"
#include "migrate.h"

void placement_work_fn_policy(struct work_struct *work)
{
	struct placement *p =
		container_of(work, struct placement, works[TI_POLICY].work);

	int cpu, eidx;
	struct perf_event *e;
	struct perf_sample s;
	struct placement_shared_counter diff = {};

	u64 ts = local_clock();
	struct placement_shared_state *state = &p->state;
	scoped_guard(vmevent, HOTNESS_IDENTIFICATION_COST);
	scoped_guard(mutex, &state->lock)
		for_each_sample_from_cpu_x_event(p, cpu, eidx, e, s)
	{
		if (!s.phys_addr)
			continue;
		u64 pfn = PFN_DOWN(s.phys_addr);
		if (unlikely(debug_migration_latency)) {
			page_debug_info_mark_found(&p->debug.pages, &s, ts);
		}
		bool in_dram = pfn_to_nid(pfn) == DRAM_NID;
		u64 count = streaming_decaying_sketch_push(&state->sds, pfn);
		struct pair elem = { pfn, count }, old = {};

		int idx = in_dram ? CHI_DEMOTION : CHI_PROMOTION;
		old = indexable_heap_insert(state->candidate + idx, &elem);
		placement_shared_counter_count(&diff, &s);
	}
	placement_shared_counter_merge(&state->counters, &diff);
	pr_info_ratelimited("%s: collected dram=%llu total=%llu samples\n",
			    __func__, diff.dram_samples, diff.total_samples);
	queue_delayed_work(system_wq, &p->works[TI_MIGRATION], 0);
}

void placement_work_fn_migration(struct work_struct *work)
{
	struct placement *p =
		container_of(work, struct placement, works[TI_MIGRATION].work);

	struct zone *dram_normal = DRAM_NODE->node_zones + ZONE_NORMAL;
	struct zone *pmem_normal = PMEM_NODE->node_zones + ZONE_NORMAL;

	struct placement_shared_state *state = &p->state;
	scoped_guard(vmevent, PAGE_MIGRATION_COST);
	scoped_guard(mutex, &state->lock)
	{
		struct placement_shared_counter counters =
			placement_shared_counter_copy(&state->counters);

		u64 target = migration_target_dram_access_percentile;
		u64 has = checked_div(counters.dram_samples * 100,
				      counters.total_samples);
		pr_info_ratelimited(
			"%s: DRAM access percentile target=%llu has=%llu (%llu/%llu)\n",
			__func__, target, has, counters.dram_samples,
			counters.total_samples);
		if (!has || has >= target) {
			return;
		}

		int err = 0;

		// demotion can always find something to do
		// so, we should skip, if the wmark permits
		if (zone_page_state(dram_normal, NR_FREE_PAGES) <
		    wmark_pages(dram_normal, WMARK_PROMO)) {
			// pr_info_ratelimited("%s: demotion\n", __func__);
			err = migration_do(&state->candidate[CHI_DEMOTION],
					   true, &p->debug.pages);
			if (err == -EAGAIN) {
				return;
			}
			if (err < migration_batch_size / 100) {
				// ++state->candidate_not_enough
				// 	  [CHI_DEMOTION];
			}
			// last_demoted = err;
		} else {
			// last_demoted += max(1ull, last_demoted / 10);
		}

		// However, promotion should be done continuously
		// pr_info_ratelimited("%s: promotion\n", __func__);
		err = migration_do(&state->candidate[CHI_PROMOTION], false,
				   &p->debug.pages);
		if (err == -EAGAIN) {
			return;
		}
		if (err < migration_batch_size / 100) {
			// ++state->candidate_not_enough[CHI_PROMOTION];
		}
	}
}
