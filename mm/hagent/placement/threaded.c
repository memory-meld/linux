#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <../../mm/internal.h>

#include "placement.h"
#include "sds.h"
#include "iheap.h"
#include "debug.h"
#include "module.h"
#include "migrate.h"

#define for_each_node_lruvec(nid, lruvec)                                   \
	for (struct mem_cgroup *memcg = mem_cgroup_iter(NULL, NULL, NULL);  \
	     memcg != NULL; memcg = mem_cgroup_iter(NULL, memcg, NULL))     \
		for ((lruvec) = mem_cgroup_lruvec(memcg, NODE_DATA((nid))); \
		     !(lruvec); (lruvec) = NULL)

#define LRU_ALL_ACTIVE (BIT(LRU_ACTIVE_FILE) | BIT(LRU_ACTIVE_ANON))
#define LRU_ALL_INACTIVE (BIT(LRU_INACTIVE_FILE) | BIT(LRU_INACTIVE_ANON))
#define LRU_ALL ((1 << NR_LRU_LISTS) - 1)

// clang-format off
#define for_each_node_lru_folio_locked(nid, lru_mask, folio)		\
	scoped_guard(lru)						\
	for (struct mem_cgroup *memcg = mem_cgroup_iter(NULL, NULL, NULL); memcg != NULL; memcg = mem_cgroup_iter(NULL, memcg, NULL))	\
	for (struct lruvec *lruvec = mem_cgroup_lruvec(memcg, NODE_DATA((nid))); lruvec; lruvec = NULL)					\
	for (enum lru_list lru = 0; lru < LRU_UNEVICTABLE; lru++)	\
	scoped_guard(spinlock_irqsave, &lruvec->lru_lock)		\
	if (!(BIT(lru) & (lru_mask))) continue;				\
	else list_for_each_entry((folio), &lruvec->lists[lru], lru)
// clang-format on

static inline u64 policy_drain_lruvec(struct streaming_decaying_sketch *sds,
				      struct indexable_heap *heap,
				      bool demotion)
{
	int source_nid = demotion ? DRAM_NID : PMEM_NID;
	// u64 lru_mask = demotion ?
	// 		       BIT(LRU_INACTIVE_FILE) | BIT(LRU_INACTIVE_ANON) :
	// 		       BIT(LRU_ACTIVE_FILE) | BIT(LRU_ACTIVE_ANON);
	u64 histo[64 + 1] = {}, scanned = 0, duplicated = 0;
	struct folio *folio;
	u64 drained = 0;
	for_each_node_lru_folio_locked(source_nid, LRU_ALL, folio)
	{
		u64 pfn = folio_pfn(folio);
		u64 count = streaming_decaying_sketch_get(sds, pfn);

		if (!!count ^ demotion) {
			struct pair elem = { pfn, count },
				    old = indexable_heap_insert(heap, &elem);
			drained += old.key == -ENOENT;
			duplicated += old.key == elem.key;
		}
		++scanned;
		++histo[count ? 1 + ilog2(count) : 0];
		if (drained / 10 > migration_batch_size) {
			goto enough;
		}
		// if (indexable_heap_length(heap) >= migration_candidate_size) {
		// 	goto enough;
		// }
	}
enough:
	pr_info_ratelimited(
		"%s: %s scanned %llu pages duplicated %llu drained %llu pages from nid=%d\n",
		__func__, demotion ? " demotion" : "promotion", scanned,
		duplicated, drained, source_nid);
	for (u64 i = 0; i < ARRAY_SIZE(histo); ++i) {
		if (histo[i]) {
			pr_info_ratelimited("%s: [%llu, %llu) = %llu\n",
					    __func__, i ? 1ull << (i - 1) : 0,
					    i ? 1ull << i : 1, histo[i]);
		}
	}
	return drained;
}

static void memcg_print_debug(struct mem_cgroup *memcg)
{
	u64 page_state[NR_LRU_LISTS + 1] = {};
	int nid;
	for_each_node_state(nid, N_MEMORY) {
		enum lru_list lru;
		scoped_guard(lru) for_each_evictable_lru(lru)
		{
			struct lruvec *lruvec =
				mem_cgroup_lruvec(memcg, NODE_DATA(nid));
			// u64 nr = lruvec_page_state_local(lruvec, NR_LRU_BASE + lru);
			// page_state[lru] += nr;
			// page_state[NR_LRU_LISTS] += nr;
			scoped_guard(spinlock_irqsave, &lruvec->lru_lock)
			{
				u64 nr = list_count_nodes(&lruvec->lists[lru]);
				page_state[lru] += nr;
				page_state[NR_LRU_LISTS] += nr;
			}
		}
	}
	pr_info("%s: memcg=0x%px inactive_anon=%llu active_anon=%llu inactive_file=%llu active_file=%llu unevictable=%llu total=%llu\n",
		__func__, memcg, page_state[LRU_INACTIVE_ANON],
		page_state[LRU_ACTIVE_ANON], page_state[LRU_INACTIVE_FILE],
		page_state[LRU_ACTIVE_FILE], page_state[LRU_UNEVICTABLE],
		page_state[NR_LRU_LISTS]);
}

static void memcg_print_debug_all(void)
{
	for (struct mem_cgroup *memcg = mem_cgroup_iter(NULL, NULL, NULL);
	     memcg != NULL; memcg = mem_cgroup_iter(NULL, memcg, NULL)) {
		memcg_print_debug(memcg);
	}
}

int placement_thread_fn_policy(struct placement *p)
{
	pr_info("%s: thread started\n", __func__);

	u64 timeout = usecs_to_jiffies(10000);
	u64 interval = 10000, iter = 0, valid = 0;

	while (!kthread_should_stop()) {
		u64 begin = local_clock();
		if (iter++ % interval == 0) {
			// intel_pmu_print_debug_all();
			// struct placement_shared_state *state = &p->state;
			// scoped_guard(mutex, &state->lock)
			// {
			// 	for (int i = 0; i < CHI_MAX; ++i)
			// 		indexable_heap_print_debug(
			// 			&state->candidate[i]);
			// }
			memcg_print_debug_all();
		}
		int cpu, eidx;
		struct perf_event *e;
		struct perf_sample s;
		struct placement_shared_counter diff = {};

		struct placement_shared_state *state = &p->state;
		u64 ts = local_clock();
		scoped_guard(mutex, &state->lock)

			for_each_sample_from_cpu_x_event(p, cpu, eidx, e, s)
		{
			if (unlikely(debug_log_samples)) {
				sample_debug_info_push(p->debug.samples, &s);
			}

			++valid;
			// pr_info_ratelimited(
			// 	"%s: got %llu-th sample: cpu=%d eidx=%d pid=%u tid=%u time=%llu addr=0x%llx weight=%llu phys_addr=0x%llx\n",
			// 	__func__, valid, cpu, eidx, s.pid, s.tid,
			// 	s.time, s.addr, s.weight, s.phys_addr);
			if (!s.phys_addr) {
				continue;
			}

			u64 pfn = PFN_DOWN(s.phys_addr);
			if (unlikely(debug_migration_latency)) {
				page_debug_info_mark_found(&p->debug.pages, &s,
							   ts);
			}
			bool in_dram = pfn_to_nid(pfn) == DRAM_NID;
			u64 count = streaming_decaying_sketch_push(&state->sds,
								   pfn);
			struct pair elem = { pfn, count }, old = {};

			int idx = in_dram ? CHI_DEMOTION : CHI_PROMOTION;
			old = indexable_heap_insert(state->candidate + idx,
						    &elem);
			// switch (old.key) {
			// case -EFBIG:
			// 	break;
			// case -ENOENT:
			// 	pr_info_ratelimited(
			// 		"%s: %s candidate insert  pfn=0x%llx count=%llu\n",
			// 		__func__,
			// 		in_dram ? " demotion" : "promotion",
			// 		pfn, count);
			// 	break;
			// default:
			// 	pr_info_ratelimited(
			// 		"%s: %s candidate %s pfn=0x%llx count=%llu old_count=%llu\n",
			// 		__func__,
			// 		in_dram ? " demotion" : "promotion",
			// 		old.key == elem.key ? "update " :
			// 				      "replace",
			// 		pfn, count, old.value);
			// 	break;
			// }
			placement_shared_counter_count(&diff, &s);
		}

		placement_shared_counter_merge(&state->counters, &diff);

		for (int i = 0; i < CHI_MAX; ++i) {
			u64 *not_eough = state->candidate_not_enough + i;
			if (*not_eough == 0) {
				continue;
			}
			// should drain_lru
			u64 drained = policy_drain_lruvec(&state->sds,
							  &state->candidate[i],
							  i == CHI_DEMOTION);
			*not_eough = saturating_sub(
				*not_eough, drained / migration_batch_size);
		}

		count_vm_events(HOTNESS_IDENTIFICATION_COST,
				local_clock() - begin);

		// give up cpu
		// pr_info_ratelimited("%s: thread giving up cpu\n", __func__);
		schedule_timeout_interruptible(timeout);
	}

	return 0;
}

static void zone_wmark_print_debug(struct zone *z)
{
	pr_info("%s: min=%lu low=%lu high=%lu promo=%lu\n", __func__,
		wmark_pages(z, WMARK_MIN), wmark_pages(z, WMARK_LOW),
		wmark_pages(z, WMARK_HIGH), wmark_pages(z, WMARK_PROMO));
}

// void cyc2ns_print_debug(void);
void tsc_sched_clock_ratio_print_debug(void *)
{
	u64 tsc = rdtsc();
	u64 lclock = local_clock();
	u64 sclock = local_clock();
	pr_info_ratelimited(
		"%s: cpu=%d tsc=%llu lclock=%llu sclock=%llu lsoffset=%llu\n",
		__func__, smp_processor_id(), tsc, lclock, sclock,
		sclock - lclock);
	// cyc2ns_print_debug();
}

int placement_thread_fn_migration(struct placement *p)
{
	pr_info("%s: thread started\n", __func__);
	u64 timeout = usecs_to_jiffies(100000);
	u64 interval = 1000, iter = 0;

	struct zone *dram_normal = DRAM_NODE->node_zones + ZONE_NORMAL;
	struct zone *pmem_normal = PMEM_NODE->node_zones + ZONE_NORMAL;

	pr_info("%s: DRAM normal zone:\n", __func__);
	zone_wmark_print_debug(dram_normal);

	pr_info("%s: PMEM normal zone:\n", __func__);
	zone_wmark_print_debug(pmem_normal);

	u64 last_demoted = migration_batch_size;
	while (!kthread_should_stop()) {
		u64 begin = local_clock();
		struct placement_shared_state *state = &p->state;
		scoped_guard(mutex, &state->lock)
		{
			struct placement_shared_counter counters =
				placement_shared_counter_copy(&state->counters);

			u64 target = migration_target_dram_access_percentile;
			u64 has = counters.dram_samples * 100 /
				  (counters.total_samples + 1);
			if (iter++ % interval == 0) {
				pr_info_ratelimited(
					"%s: DRAM access percentile target=%llu has=%llu (%llu/%llu)\n",
					__func__, target, has,
					counters.dram_samples,
					counters.total_samples);
			}
			if (!has || has >= target) {
				goto yield;
			}

			int err = 0;

			// demotion can always find something to do
			// so, we should skip, if the wmark permits
			if (zone_page_state(dram_normal, NR_FREE_PAGES) <
			    wmark_pages(dram_normal, WMARK_PROMO)) {
				// pr_info_ratelimited("%s: demotion\n", __func__);
				err = migration_do(
					&state->candidate[CHI_DEMOTION], true,
					&p->debug.pages);
				if (err == -EAGAIN) {
					goto yield;
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
			err = migration_do(&state->candidate[CHI_PROMOTION],
					   false, &p->debug.pages);
			if (err == -EAGAIN) {
				goto yield;
			}
			if (err < migration_batch_size / 100) {
				// ++state->candidate_not_enough[CHI_PROMOTION];
			}
		}

yield:
		count_vm_events(PAGE_MIGRATION_COST, local_clock() - begin);
		// give up cpu
		// pr_info_ratelimited("%s: thread giving up cpu\n", __func__);
		schedule_timeout_interruptible(timeout);
		smp_call_function(tsc_sched_clock_ratio_print_debug, NULL,
				  true);
	}
	return 0;
}
