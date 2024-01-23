#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/migrate.h>
#include <../../mm/internal.h>

#include "placement.h"
#include "sds.h"
#include "iheap.h"
#include "histogram.h"
#include "debug.h"
#include "module.h"

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

#define for_each_sample_from_cpu_x_event(p, cpu, eidx, e, s)                  \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu)                         \
		for (eidx = 0; eidx < EI_MAX; ++eidx)                         \
			for (e = (p)->events[cpu][eidx]; e; e = NULL)         \
				for (; !spsc_pop(e->overflow_handler_context, \
						 &s, sizeof(s));)

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

enum folio_debug_info_timestamp {
	// Used for calculating the latency between migration and pebs hardware
	// first generated the sample
	FDTI_FIRST_FOUND,
	FDTI_COLLECTION,
	FDTI_MIGRATION,
	// Used for calculating how long does it take for the same sample to
	// occur again
	FDTI_LAST_FOUND,
	FDTI_MAX,
};

struct page_debug_info {
	struct {
		u64 total_accesses;
		u64 timestamps[FDTI_MAX];
	} *pages;
	u64 len;
	struct logarithmic_histogram reoccurrence_latency;
};
static inline void page_debug_info_drop(struct page_debug_info *info)
{
	kvfree(info->pages);
	memset(info, 0, sizeof(*info));
}
static int __must_check page_debug_info_init(struct page_debug_info *info)
{
	info->len = num_possible_pages();
	info->pages =
		TRY(kvcalloc(info->len, sizeof(*info->pages), GFP_KERNEL));
	return 0;
}
static int page_debug_info_mark_found(struct page_debug_info *info,
				      struct perf_sample *s, u64 now)
{
	if (!info->len || !info->pages) {
		return -EINVAL;
	}
	u64 pfn = PFN_DOWN(s->phys_addr);
	if (!pfn) {
		return -ENOENT;
	}
	typeof(info->pages) one = info->pages + pfn;
	one->total_accesses += 1;
	if (!one->timestamps[FDTI_FIRST_FOUND]) {
		one->timestamps[FDTI_FIRST_FOUND] = s->time;
	}
	if (one->timestamps[FDTI_LAST_FOUND]) {
		u64 reoccurrence = saturating_sub(
			s->time, one->timestamps[FDTI_LAST_FOUND]);
		logarithmic_histogram_push(&info->reoccurrence_latency,
					   reoccurrence);
	}
	one->timestamps[FDTI_LAST_FOUND] = s->time;
	if (!one->timestamps[FDTI_COLLECTION]) {
		one->timestamps[FDTI_COLLECTION] = now;
	}
	return 0;
}
static int page_debug_info_mark_migrated(struct page_debug_info *info, u64 pfn,
					 u64 now)
{
	if (!pfn) {
		return -ENOENT;
	}
	typeof(info->pages) one = info->pages + pfn;
	if (!one->timestamps[FDTI_MIGRATION]) {
		one->timestamps[FDTI_MIGRATION] = now;
	}
	return 0;
}
static void page_debug_info_print_debug(struct page_debug_info *info)
{
	struct logarithmic_histogram total_accesses = {},
				     collection_latency = {},
				     migration_latency = {};
	for (u64 i = 0; i < info->len; ++i) {
		typeof(info->pages) one = info->pages + i;
		logarithmic_histogram_push(&total_accesses,
					   one->total_accesses);
		if (!one->timestamps[FDTI_FIRST_FOUND] ||
		    !one->timestamps[FDTI_COLLECTION]) {
			continue;
		}
		logarithmic_histogram_push(
			&collection_latency,
			saturating_sub(one->timestamps[FDTI_COLLECTION],
				       one->timestamps[FDTI_FIRST_FOUND]));
		if (!one->timestamps[FDTI_MIGRATION]) {
			continue;
		}
		logarithmic_histogram_push(
			&migration_latency,
			saturating_sub(one->timestamps[FDTI_MIGRATION],
				       one->timestamps[FDTI_COLLECTION]));
	}
	pr_info("total_accesses histo:\n");
	logarithmic_histogram_print_debug(&total_accesses);
	pr_info("collection_latency histo:\n");
	logarithmic_histogram_print_debug(&collection_latency);
	pr_info("migration_latency histo:\n");
	logarithmic_histogram_print_debug(&migration_latency);
	pr_info("reoccurrence_latency histo:\n");
	logarithmic_histogram_print_debug(&info->reoccurrence_latency);
}

struct sample_debug_info {
	u64 size, cap;
	struct perf_sample data[0];
};
static inline void sample_debug_info_drop(struct sample_debug_info *info)
{
	kvfree(info);
}
static int __must_check sample_debug_info_init(struct sample_debug_info **info,
					       u64 cap)
{
	*info = kvzalloc(sizeof(**info) + sizeof(struct perf_sample) * cap,
			 GFP_KERNEL);
	if (!*info) {
		return -ENOMEM;
	}
	(*info)->cap = cap;
	return 0;
}
static int sample_debug_info_push(struct sample_debug_info *info,
				  struct perf_sample *s)
{
	if (info->size >= info->cap) {
		return -ENOSPC;
	}
	info->data[info->size++] = *s;
	return 0;
}
static ssize_t sample_debug_info_write_file(struct sample_debug_info *info,
					    char const *filename)
{
	return debug_write_file(filename, info->data,
				info->size * sizeof(struct perf_sample));
}

struct page_debug_info page_debug_info = {};
int placement_thread_fn_policy(struct placement *p)
{
	pr_info("%s: thread started\n", __func__);

	if (debug_migration_latency) {
		TRY(page_debug_info_init(&page_debug_info));
	}
	struct sample_debug_info *sample_debug_info = {};
	if (debug_log_samples) {
		TRY(sample_debug_info_init(&sample_debug_info, 2ul << 20));
	}

	u64 timeout = usecs_to_jiffies(10000);
	u64 interval = 10000, iter = 0, valid = 0;

	struct streaming_decaying_sketch sds;
	streaming_decaying_sketch_update_param();
	TRY(streaming_decaying_sketch_init(&sds,
					   streaming_decaying_sketch_width,
					   streaming_decaying_sketch_depth));

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
		scoped_guard(mutex, &state->lock)
		{
			u64 ts = local_clock();
			for_each_sample_from_cpu_x_event(p, cpu, eidx, e, s)
			{
				if (unlikely(debug_log_samples)) {
					sample_debug_info_push(
						sample_debug_info, &s);
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
					page_debug_info_mark_found(
						&page_debug_info, &s, ts);
				}
				bool in_dram = pfn_to_nid(pfn) == DRAM_NID;
				u64 count = streaming_decaying_sketch_push(&sds,
									   pfn);
				struct pair elem = { pfn, count }, old = {};

				int idx = in_dram ? CHI_DEMOTION :
						    CHI_PROMOTION;
				old = indexable_heap_insert(
					state->candidate + idx, &elem);
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
				u64 *not_eough =
					state->candidate_not_enough + i;
				if (*not_eough == 0) {
					continue;
				}
				// should drain_lru
				u64 drained = policy_drain_lruvec(
					&sds, &state->candidate[i],
					i == CHI_DEMOTION);
				*not_eough = saturating_sub(
					*not_eough,
					drained / migration_batch_size);
			}
		}
		count_vm_events(HOTNESS_IDENTIFICATION_COST,
				local_clock() - begin);

		// give up cpu
		// pr_info_ratelimited("%s: thread giving up cpu\n", __func__);
		schedule_timeout_interruptible(timeout);
	}

	if (debug_log_samples) {
		sample_debug_info_write_file(sample_debug_info,
					     "/out/debug_samples");
		sample_debug_info_drop(sample_debug_info);
	}
	if (debug_migration_latency) {
		page_debug_info_print_debug(&page_debug_info);
		page_debug_info_drop(&page_debug_info);
	}

	return 0;
}

static void zone_wmark_print_debug(struct zone *z)
{
	pr_info("%s: min=%lu low=%lu high=%lu promo=%lu\n", __func__,
		wmark_pages(z, WMARK_MIN), wmark_pages(z, WMARK_LOW),
		wmark_pages(z, WMARK_HIGH), wmark_pages(z, WMARK_PROMO));
}

// reference:
//  - migrate_folio_unmap() in mm/migrate.c
//  - folio_get_anon_vma() in mm/rmap.c
//  - rmap_walk_anon() in mm/rmap.c
//  - migrate_to_node() in mm/mempolicy.c
//  - queue_folios_pte_range() in mm/mempolicy.c
static int migration_isolate_folios(struct indexable_heap *heap, bool demotion,
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

static int migration_migrate_folios(struct list_head *isolated, bool demotion)
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

static int migration_do(struct indexable_heap *heap, bool demotion)
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
			page_debug_info_mark_migrated(&page_debug_info, pfns[i],
						      ts);
		}
	}
	// kvfree() will do nothing for a NULL pointer
	kvfree(pfns);
	return migrated;
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
					&state->candidate[CHI_DEMOTION], true);
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
					   false);
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
