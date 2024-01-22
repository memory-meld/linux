#ifndef HAGENT_PLACEMENT_PLACEMENT_H
#define HAGENT_PLACEMENT_PLACEMENT_H

#include "iheap.h"
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
}

static inline int placement_shared_state_init(struct placement_shared_state *s)
{
	mutex_init(&s->lock);
	s->counters = (struct placement_shared_counter){};
	indexable_heap_update_param();
	// This is correct, we use max_heap to keep the smallest counts for DRAM
	for (int i = 0; i < CHI_MAX; ++i)
		BUG_ON(indexable_heap_init(&s->candidate[i], i == CHI_DEMOTION,
					   migration_candidate_size));
	return 0;
}

struct placement {
	struct perf_event *events[NR_CPUS][EI_MAX];
	struct spsc chan[NR_CPUS][EI_MAX];
	struct task_struct *threads[TI_MAX];
	struct placement_shared_state state;
};

#define for_each_cpu_x_event(p, cpu, eidx, e)         \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu) \
		for (eidx = 0; eidx < EI_MAX; ++eidx) \
			for (e = (p)->events[cpu][eidx]; e; e = NULL)

#define for_each_cpu_x_event_ptr(p, cpu, eidx, eptr)  \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu) \
		for (eidx = 0; eidx < EI_MAX; ++eidx) \
			for (eptr = &(p)->events[cpu][eidx]; eptr; eptr = NULL)

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
		struct spsc *ch = e->overflow_handler_context;
		perf_event_release_kernel(e);
		spsc_drop(ch);
	}
}

static inline void placement_event_overflow(struct perf_event *event,
					    struct perf_sample_data *data,
					    struct pt_regs *regs)
{
	void perf_prepare_sample(struct perf_sample_data * data,
				 struct perf_event * event,
				 struct pt_regs * regs);
	// for locking see: __perf_event_output
	scoped_guard(rcu)
	{
		scoped_guard(irqsave)
		{
			struct spsc *ch = event->overflow_handler_context;
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
	}
}

static inline int placement_event_init(struct placement *p)
{
	event_attrs_update_param();
	int cpu, i;
	struct perf_event **e;
	for_each_cpu_x_event_ptr(p, cpu, i, e)
	{
		struct spsc *ch = &p->chan[cpu][i];
		int err =
			spsc_init(ch, SPSC_NELEMS * sizeof(struct perf_sample));
		if (err) {
			placement_event_drop(p);
			return err;
		}

		*e = perf_event_create_kernel_counter(&event_attrs[i], cpu,
						      NULL,
						      placement_event_overflow,
						      ch);
		if (IS_ERR(*e)) {
			placement_event_drop(p);
			return PTR_ERR(*e);
		}
	}

	return 0;
}

static inline int placement_thread_start(struct placement *p)
{
	for (int i = 0; i < TI_MAX; ++i) {
		struct task_struct *t = p->threads[i];
		if (!t) {
			continue;
		}
		wake_up_process(t);
	}
	return 0;
}

static inline void placement_thread_stop(struct placement *p)
{
	for (int i = 0; i < TI_MAX; ++i) {
		struct task_struct *t = p->threads[i];
		if (!t)
			continue;
		// Scynchrously stop the thread and wait for exit
		kthread_stop(t);
	}
}

static inline void placement_thread_drop(struct placement *p)
{
	placement_thread_stop(p);
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

static inline void placement_drop(struct placement *p)
{
	placement_thread_drop(p);
	placement_event_drop(p);
	placement_shared_state_drop(&p->state);
}

static inline int placement_init(struct placement *p)
{
	memset(p, 0, sizeof(*p));
	TRY(placement_event_init(p));
	TRY(placement_thread_init(p));
	TRY(placement_shared_state_init(&p->state));

	return 0;
}

#endif // !HAGENT_PLACEMENT_PLACEMENT_H
