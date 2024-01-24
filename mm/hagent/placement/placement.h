#ifndef HAGENT_PLACEMENT_PLACEMENT_H
#define HAGENT_PLACEMENT_PLACEMENT_H

#include "sds.h"
#include "iheap.h"
#include "debug.h"
#include "module.h"
#include "pebs.h"
#include "spsc.h"
#include "utils.h"

enum event_index {
	EI_READ = 0,
	// EI_WRITE = 1,
	EI_MAX,
};

enum thread_index {
	TI_POLICY,
	TI_MIGRATION,
	TI_MAX,
};

enum events {
	MEM_TRANS_RETIRED_LOAD_LATENCY = 0x01cd,
	MEM_INST_RETIRED_ALL_STORES = 0x82d0,
};

static struct perf_event_attr event_attrs[EI_MAX] = {
	[EI_READ] = {
		.type = PERF_TYPE_RAW,
		.config = MEM_TRANS_RETIRED_LOAD_LATENCY,
		.config1 = LOAD_LATENCY_THRESHOLD,
		.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR |
			       PERF_SAMPLE_WEIGHT | PERF_SAMPLE_PHYS_ADDR,
		.sample_period = SAMPLE_PERIOD,
		.precise_ip = 3,
		.disabled = 1,
		.exclude_kernel = 1,
		.exclude_hv = 1,
		.exclude_callchain_kernel= 1,
	},
	// [EI_WRITE] = {
	// 	.type = PERF_TYPE_RAW,
	// 	.config = MEM_INST_RETIRED_ALL_STORES,
	// 	.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR |
	// 		       PERF_SAMPLE_WEIGHT | PERF_SAMPLE_PHYS_ADDR,
	// 	.sample_period = SAMPLE_PERIOD,
	// 	.precise_ip = 3,
	// 	.disabled = 1,
	// 	.exclude_kernel = 1,
	// 	.exclude_hv = 1,
	// 	.exclude_callchain_kernel= 1,
	// },
};
static u64 event_attrs_index(struct perf_event_attr *attr)
{
	for (u64 i = 0; i < EI_MAX; ++i) {
		if (attr->config == event_attrs[i].config) {
			return i;
		}
	}
	BUG();
}

static inline void event_attrs_update_param(void)
{
	event_attrs[EI_READ].config1 = load_latency_threshold;
	event_attrs[EI_READ].sample_period = load_latency_sample_period;
	// event_attrs[EI_WRITE].sample_period = retired_stores_sample_period;
}

struct placement_shared_counter {
	u64 total_samples, dram_samples, pmem_samples;
};

static inline void
placement_shared_counter_count(struct placement_shared_counter *state,
			       struct perf_sample *sample)
{
	if (!sample->phys_addr) {
		return;
	}
	++state->total_samples;
	u64 pfn = PFN_DOWN(sample->phys_addr);
	bool in_dram = pfn_to_nid(pfn) == DRAM_NID;
	if (in_dram) {
		++state->dram_samples;
	} else {
		++state->pmem_samples;
	}
}

static inline void
placement_shared_counter_merge(struct placement_shared_counter *state,
			       struct placement_shared_counter *diff)
{
#define __INC_ONCE(x, val)                            \
	do {                                          \
		*(volatile typeof(x) *)&(x) += (val); \
	} while (0)

	__INC_ONCE(state->total_samples, diff->total_samples);
	__INC_ONCE(state->dram_samples, diff->dram_samples);
	__INC_ONCE(state->pmem_samples, diff->pmem_samples);

#undef __INC_ONCE
}

static inline struct placement_shared_counter
placement_shared_counter_copy(struct placement_shared_counter *state)
{
	struct placement_shared_counter ret = {
		.total_samples = READ_ONCE(state->total_samples),
		.dram_samples = READ_ONCE(state->dram_samples),
		.pmem_samples = READ_ONCE(state->pmem_samples),
	};
	return ret;
}

enum placement_shared_state_chan_index {
	CHI_DEMOTION,
	CHI_PROMOTION,
	CHI_MAX,
};

struct placement_shared_state {
	struct streaming_decaying_sketch sds;
	struct mutex lock;
	struct placement_shared_counter counters;
	struct indexable_heap candidate[CHI_MAX];
	// struct spsc *chan[CHI_MAX];
	u64 candidate_not_enough[CHI_MAX];
};

static inline void placement_shared_state_drop(struct placement_shared_state *s)
{
	for (int i = 0; i < CHI_MAX; ++i)
		indexable_heap_drop(&s->candidate[i]);
	mutex_destroy(&s->lock);
	streaming_decaying_sketch_drop(&s->sds);
}

static inline int placement_shared_state_init(struct placement_shared_state *s)
{
	streaming_decaying_sketch_update_param();
	TRY(streaming_decaying_sketch_init(&s->sds,
					   streaming_decaying_sketch_width,
					   streaming_decaying_sketch_depth));
	mutex_init(&s->lock);
	s->counters = (struct placement_shared_counter){};
	indexable_heap_update_param();
	// This is correct, we use max_heap to keep the smallest counts for DRAM
	for (int i = 0; i < CHI_MAX; ++i)
		BUG_ON(indexable_heap_init(&s->candidate[i], i == CHI_DEMOTION,
					   migration_candidate_size));
	return 0;
}

struct placement_debug {
	struct page_debug_info pages;
	struct sample_debug_info *samples;
};
static inline void placement_debug_drop(struct placement_debug *d)
{
	if (debug_log_samples) {
		sample_debug_info_write_file(d->samples, "/out/debug_samples");
		sample_debug_info_drop(d->samples);
	}
	if (debug_migration_latency) {
		page_debug_info_print_debug(&d->pages);
		page_debug_info_drop(&d->pages);
	}
}
static inline __must_check int placement_debug_init(struct placement_debug *d)
{
	if (debug_migration_latency) {
		TRY(page_debug_info_init(&d->pages));
	}
	if (debug_log_samples) {
		TRY(sample_debug_info_init(&d->samples, 2ul << 20));
	}
	return 0;
}

struct placement {
	struct perf_event *events[NR_CPUS][EI_MAX];
	struct spsc chan[NR_CPUS][EI_MAX];
	union {
		struct task_struct *threads[TI_MAX];
		struct delayed_work works[TI_MAX];
	};
	struct placement_shared_state state;
	struct placement_debug debug;
};

#define for_each_cpu_x_event_nocheck(p, cpu, eidx, e) \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu) \
		for (eidx = 0; eidx < EI_MAX; ++eidx)

#define for_each_cpu_x_event(p, cpu, eidx, e)         \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu) \
		for (eidx = 0; eidx < EI_MAX; ++eidx) \
			for (e = (p)->events[cpu][eidx]; e; e = NULL)

#define for_each_cpu_x_event_ptr(p, cpu, eidx, eptr)  \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu) \
		for (eidx = 0; eidx < EI_MAX; ++eidx) \
			for (eptr = &(p)->events[cpu][eidx]; eptr; eptr = NULL)

#define for_each_sample_from_cpu_x_event(p, cpu, eidx, e, s)                \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu)                       \
		for (eidx = 0; eidx < EI_MAX; ++eidx)                       \
			for (e = (p)->events[cpu][eidx]; e; e = NULL)       \
				for (; !spsc_pop(&(p)->chan[cpu][eidx], &s, \
						 sizeof(s));)

static inline void placement_spsc_drop(struct placement *p)
{
	int cpu, i;
	struct perf_event *e;
	for_each_cpu_x_event_nocheck(p, cpu, i, e)
	{
		struct spsc *ch = &p->chan[cpu][i];
		if (ch) {
			spsc_drop(ch);
		}
	}
}

static inline int placement_spsc_init(struct placement *p)
{
	int cpu, i;
	struct perf_event *e;
	for_each_cpu_x_event_nocheck(p, cpu, i, e)
	{
		struct spsc *ch = &p->chan[cpu][i];
		TRY(spsc_init(ch, SPSC_NELEMS * sizeof(struct perf_sample)));
	}
	return 0;
}

static inline int placement_event_start(struct placement *p)
{
	int cpu, i;
	struct perf_event *e;
	for_each_cpu_x_event(p, cpu, i, e)
	{
		perf_event_enable(e);
	}
	return 0;
}

static inline void placement_event_stop(struct placement *p)
{
	int cpu, i;
	struct perf_event *e;
	for_each_cpu_x_event(p, cpu, i, e)
	{
		perf_event_disable(e);
	}
}

static inline void placement_event_drop(struct placement *p)
{
	int cpu, i;
	struct perf_event *e;
	for_each_cpu_x_event(p, cpu, i, e)
	{
		perf_event_release_kernel(e);
	}
}

static inline void placement_event_overflow(struct perf_event *event,
					    struct perf_sample_data *data,
					    struct pt_regs *regs);
static inline int placement_event_init(struct placement *p)
{
	event_attrs_update_param();
	int cpu, i;
	struct perf_event **e;
	for_each_cpu_x_event_ptr(p, cpu, i, e)
	{
		*e = perf_event_create_kernel_counter(&event_attrs[i], cpu,
						      NULL,
						      placement_event_overflow,
						      p);
		if (IS_ERR(*e)) {
			placement_event_drop(p);
			return PTR_ERR(*e);
		}
	}

	return 0;
}

extern int placement_thread_fn_policy(struct placement *p);
extern int placement_thread_fn_migration(struct placement *p);
static int (*placement_thread_fn[TI_MAX])(void *) = {
	[TI_POLICY] = (int (*)(void *))placement_thread_fn_policy,
	[TI_MIGRATION] = (int (*)(void *))placement_thread_fn_migration,
};
static char *placement_thread_name[TI_MAX] = {
	[TI_POLICY] = "placement_policy",
	[TI_MIGRATION] = "placement_migration",
};
// static int placement_thread_nice[TI_MAX] = {
// 	[TI_POLICY] = 1,
// 	[TI_MIGRATION] = -1,
// };

static inline int placement_thread_start(struct placement *p)
{
	for (int i = 0; i < TI_MAX; ++i) {
		struct task_struct *t = p->threads[i];
		if (!t) {
			continue;
		}
		pr_info("%s: wake_up_process(%s)\n", __func__,
			placement_thread_name[i]);
		wake_up_process(t);
	}
	return 0;
}

static inline void placement_thread_stop(struct placement *p)
{
	for (int i = TI_MAX; i > 0; --i) {
		struct task_struct *t = p->threads[i - 1];
		if (!t)
			continue;
		pr_info("%s: kthread_stop(%s)\n", __func__,
			placement_thread_name[i - 1]);
		// Scynchrously stop the thread and wait for exit
		kthread_stop(t);
	}
}

static inline void placement_thread_drop(struct placement *p)
{
	placement_thread_stop(p);
}

extern void placement_work_fn_policy(struct work_struct *);
extern void placement_work_fn_migration(struct work_struct *);
static void (*placement_work_fn[TI_MAX])(struct work_struct *) = {
	[TI_POLICY] = placement_work_fn_policy,
	[TI_MIGRATION] = placement_work_fn_migration,
};
static char *placement_work_name[TI_MAX] = {
	[TI_POLICY] = "placement_policy",
	[TI_MIGRATION] = "placement_migration",
};

static inline int __must_check placement_thread_init(struct placement *p)
{
	for (int i = 0; i < TI_MAX; ++i) {
		struct task_struct *t = kthread_create(
			placement_thread_fn[i], p, placement_thread_name[i]);
		pr_info("%s: kthread_create(%s) = 0x%px\n", __func__,
			placement_thread_name[i], t);
		if (IS_ERR(t)) {
			placement_thread_drop(p);
			return PTR_ERR(t);
		} else {
			// sched_set_normal(t, placement_thread_nice[i]);
			p->threads[i] = t;
		}
	}

	return 0;
}

static inline int __must_check placement_work_init(struct placement *p)
{
	for (int i = 0; i < TI_MAX; ++i) {
		INIT_DELAYED_WORK(&p->works[i], placement_work_fn[i]);
		pr_info("%s: INIT_DELAYED_WORK(%s, 0x%px)\n", __func__,
			placement_work_name[i], placement_work_fn[i]);
	}
	return 0;
}

static inline void placement_work_drop(struct placement *p)
{
	for (int i = 0; i < TI_MAX; ++i) {
		pr_info("%s: cancel_delayed_work_sync(%s)\n", __func__,
			placement_work_name[i]);
		// Scynchrously stop the work and wait for exit
		cancel_delayed_work_sync(&p->works[i]);
	}
}

static inline void placement_event_overflow(struct perf_event *event,
					    struct perf_sample_data *data,
					    struct pt_regs *regs)
{
	void perf_prepare_sample(struct perf_sample_data * data,
				 struct perf_event * event,
				 struct pt_regs * regs);
	struct placement *p = event->overflow_handler_context;
	u64 ith = regs->cx;

	// for locking see: __perf_event_output
	scoped_guard(rcu) scoped_guard(irqsave)
	{
		struct spsc *ch = &p->chan[smp_processor_id()]
					  [event_attrs_index(&event->attr)];
		perf_prepare_sample(data, event, regs);
		struct perf_sample s = {
			.pid = data->tid_entry.pid,
			.tid = data->tid_entry.tid,
			.time = data->time,
			.addr = data->addr,
			.weight = data->weight.full,
			.phys_addr = data->phys_addr,
		};
		if (spsc_push(ch, &s, sizeof(s))) {
			pr_warn_ratelimited(
				"%s: discard sample due to ring buffer overflow\n",
				__func__);
		}
	}
	if (static_branch_likely(&use_asynchronous_architecture) && ith == 0) {
		// pr_info_ratelimited("%s: queue_work(%s)\n", __func__,
		// 		    placement_work_name[TI_POLICY]);
		queue_delayed_work(system_wq, &p->works[TI_POLICY], 1);
	}
}

static inline void placement_drop(struct placement *p)
{
	placement_event_drop(p);
	if (static_branch_likely(&use_asynchronous_architecture)) {
		placement_work_drop(p);
	} else {
		placement_thread_drop(p);
	}
	placement_spsc_drop(p);
	placement_debug_drop(&p->debug);
	placement_shared_state_drop(&p->state);
}

static inline int placement_init(struct placement *p)
{
	memset(p, 0, sizeof(*p));
	TRY(placement_spsc_init(p));
	TRY(placement_debug_init(&p->debug));
	TRY(placement_event_init(p));
	if (asynchronous_architecture) {
		static_branch_enable(&use_asynchronous_architecture);
		pr_info("%s: use asynchronous architecture\n", __func__);
		TRY(placement_work_init(p));
	} else {
		static_branch_disable(&use_asynchronous_architecture);
		pr_info("%s: use threaded architecture\n", __func__);
		TRY(placement_thread_init(p));
	}
	TRY(placement_shared_state_init(&p->state));

	return 0;
}

static inline int placement_start(struct placement *p)
{
	TRY(placement_event_start(p));
	if (!static_branch_likely(&use_asynchronous_architecture)) {
		TRY(placement_thread_start(p));
	}
	return 0;
}

#endif // !HAGENT_PLACEMENT_PLACEMENT_H
