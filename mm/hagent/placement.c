#include <linux/module.h>
#include <linux/perf_event.h>

#include <../../kernel/events/internal.h>

#define TRY(exp)                                                              \
	({                                                                    \
		__typeof__((exp)) __err = (exp);                              \
		if ((u64)(__err) >= (u64)(-MAX_ERRNO)) {                      \
			pr_err_ratelimited("%s:%d failed with error %lld:\n", \
					   __FILE__, __LINE__, (s64)__err);   \
			dump_stack();                                         \
			return (s64)(__err);                                  \
		}                                                             \
		__err;                                                        \
	})

struct spsc_elem {
	u32 pid, tid;
	u64 time;
	u64 addr;
	u64 weight;
	u64 phys_addr;
};

enum param_defaults {
	RING_BUFFER_PAGES = 8ul << 20 >> PAGE_SHIFT,
	SAMPLE_PERIOD = 17,
	LOAD_LATENCY_THRESHOLD = 64,
	SPSC_NELEMS = (2ul << 20) / sizeof(struct spsc_elem),
};

unsigned long ring_buffer_pages = RING_BUFFER_PAGES;
module_param_named(ring_buffer_pages, ring_buffer_pages, ulong, 0644);
MODULE_PARM_DESC(
	ring_buffer_pages,
	"Number of pages allocated for the ring buffer, defaults to 1M");

unsigned long load_latency_sample_period = SAMPLE_PERIOD;
module_param_named(load_latency_sample_period, load_latency_sample_period,
		   ulong, 0644);
MODULE_PARM_DESC(load_latency_sample_period,
		 "Sample period for ldlat event, defaults to 17");

unsigned long load_latency_threshold = LOAD_LATENCY_THRESHOLD;
module_param_named(load_latency_threshold, load_latency_threshold, ulong, 0644);
MODULE_PARM_DESC(load_latency_threshold,
		 "Load latency threshold for ldlat event, defaults to 64");

unsigned long retired_stores_sample_period = SAMPLE_PERIOD;
module_param_named(retired_stores_sample_period, retired_stores_sample_period,
		   ulong, 0644);
MODULE_PARM_DESC(retired_stores_sample_period,
		 "Sample period for retired stores event, defaults to 17");

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

struct perf_event_attr event_attrs[EI_MAX] = {
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

void event_attrs_update(void)
{
	event_attrs[EI_READ].config1 = load_latency_threshold;
	event_attrs[EI_READ].sample_period = load_latency_sample_period;
	// event_attrs[EI_WRITE].sample_period = retired_stores_sample_period;
}

struct spsc {
	u64 head, tail, size;
	void *buffer;
};

static void spsc_drop(struct spsc *ch)
{
	kvfree(ch->buffer);
}

static int spsc_init(struct spsc *ch, u64 size)
{
	ch->head = ch->tail = 0;
	ch->size = size;
	ch->buffer = kvmalloc(size, GFP_KERNEL);
	if (!ch->buffer) {
		return -ENOMEM;
	}
	return 0;
}

// We will assume the buffer size is the multiple of the element size
// Producer side only modify the head pointer.
static int spsc_push(struct spsc *ch, void *buf, u64 len)
{
	u64 head = READ_ONCE(ch->head), diff = head - READ_ONCE(ch->tail);
	if (diff && diff % ch->size == 0) {
		return -ENOMEM;
	}
	memcpy(ch->buffer + head % ch->size, buf, len);
	WRITE_ONCE(ch->head, head + len);
	return 0;
}

// Consumer side only modify the tail pointer.
static int spsc_pop(struct spsc *ch, void *buf, u64 len)
{
	u64 tail = READ_ONCE(ch->tail);
	if (READ_ONCE(ch->head) == tail) {
		return -EAGAIN;
	}
	memcpy(buf, ch->buffer + tail % ch->size, len);
	WRITE_ONCE(ch->tail, tail + len);
	return 0;
}

struct placement {
	struct perf_event *events[NR_CPUS][EI_MAX];
	struct spsc chan[NR_CPUS][EI_MAX];
	struct task_struct *threads[TI_MAX];
};

#define for_each_cpu_x_event(p, cpu, eidx, e)         \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu) \
		for (eidx = 0; eidx < EI_MAX; ++eidx) \
			for (e = (p)->events[cpu][eidx]; e; e = NULL)

#define for_each_cpu_x_event_ptr(p, cpu, eidx, eptr)  \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu) \
		for (eidx = 0; eidx < EI_MAX; ++eidx) \
			for (eptr = &(p)->events[cpu][eidx]; eptr; eptr = NULL)

#define for_each_sample_from_cpu_x_event(p, cpu, eidx, e, s)                  \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu)                         \
		for (eidx = 0; eidx < EI_MAX; ++eidx)                         \
			for (e = (p)->events[cpu][eidx]; e; e = NULL)         \
				for (; !spsc_pop(e->overflow_handler_context, \
						 &s, sizeof(s));)

static int placement_event_start(struct placement *p)
{
	int cpu, i;
	struct perf_event *e;
	for_each_cpu_x_event(p, cpu, i, e)
	{
		perf_event_enable(e);
	}
	return 0;
}

static void placement_event_stop(struct placement *p)
{
	int cpu, i;
	struct perf_event *e;
	for_each_cpu_x_event(p, cpu, i, e)
	{
		perf_event_disable(e);
	}
}

static void placement_event_drop(struct placement *p)
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

static void placement_event_overflow(struct perf_event *event,
				     struct perf_sample_data *data,
				     struct pt_regs *regs)
{
	void perf_prepare_sample(struct perf_sample_data * data,
				 struct perf_event * event,
				 struct pt_regs * regs);
	scoped_guard(rcu)
	{
		scoped_guard(irqsave)
		{
			struct spsc *ch = event->overflow_handler_context;
			perf_prepare_sample(data, event, regs);
			struct spsc_elem s = {
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

static int placement_event_init(struct placement *p)
{
	event_attrs_update();
	int cpu, i;
	struct perf_event **e;
	for_each_cpu_x_event_ptr(p, cpu, i, e)
	{
		struct spsc *ch = &p->chan[cpu][i];
		int err = spsc_init(ch, SPSC_NELEMS * sizeof(struct spsc_elem));
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

static void intel_pmu_print_debug_all(void)
{
	int cpu;
	for_each_online_cpu(cpu) {
		void perf_event_print_debug(void);
		smp_call_on_cpu(cpu, (int (*)(void *))perf_event_print_debug,
				NULL, false);
	}
}

static int placement_thread_fn_policy(struct placement *p)
{
	pr_info("%s: thread started\n", __func__);
	u64 timeout = usecs_to_jiffies(1000);
	u64 interval = 10000, iter = 0, valid = 0;
	while (!kthread_should_stop()) {
		if (iter++ % interval == 0) {
			// intel_pmu_print_debug_all();
		}
		int cpu, eidx;
		struct perf_event *e;
		struct spsc_elem s;
		for_each_sample_from_cpu_x_event(p, cpu, eidx, e, s)
		{
			++valid;
			pr_info_ratelimited(
				"%s: got %llu-th sample: cpu=%d eidx=%d pid=%u tid=%u time=%llu addr=0x%llx weight=%llu phys_addr=0x%llx\n",
				__func__, valid, cpu, eidx, s.pid, s.tid,
				s.time, s.addr, s.weight, s.phys_addr);
		}
		// give up cpu
		schedule_timeout_interruptible(timeout);
	}
	return 0;
}

static int placement_thread_fn_migration(struct placement *p)
{
	pr_info("%s: thread started\n", __func__);
	u64 timeout = usecs_to_jiffies(1000);
	while (!kthread_should_stop()) {
		// give up cpu
		schedule_timeout_interruptible(timeout);
	}
	return 0;
}

static int placement_thread_start(struct placement *p)
{
	for (int i = 0; i < EI_MAX; ++i) {
		struct task_struct *t = p->threads[i];
		if (!t)
			continue;
		TRY(wake_up_process(t));
	}
	return 0;
}

static void placement_thread_stop(struct placement *p)
{
	for (int i = 0; i < TI_MAX; ++i) {
		struct task_struct *t = p->threads[i];
		if (!t)
			continue;
		// Scynchrously stop the thread and wait for exit
		kthread_stop(t);
	}
}

static void placement_thread_drop(struct placement *p)
{
	placement_thread_stop(p);
}

static int placement_thread_init(struct placement *p)
{
	for (int i = 0; i < TI_MAX; ++i) {
		struct task_struct **t = &p->threads[i];
		switch (i) {
		case TI_POLICY:
			*t = kthread_create(
				(int (*)(void *))placement_thread_fn_policy, p,
				"placement_policy");
			break;
		case TI_MIGRATION:
			*t = kthread_create(
				(int (*)(void *))placement_thread_fn_migration,
				p, "placement_migration");
			break;
		default:
			return -EINVAL;
		}

		if (IS_ERR(t)) {
			placement_thread_drop(p);
			return PTR_ERR(t);
		}
	}

	return 0;
}

static void placement_drop(struct placement *p)
{
	placement_thread_drop(p);
	placement_event_drop(p);
}

static int placement_init(struct placement *p)
{
	memset(p, 0, sizeof(*p));
	TRY(placement_event_init(p));
	TRY(placement_thread_init(p));

	return 0;
}

static struct placement __global_placement;

static int init(void)
{
	struct placement *p = &__global_placement;
	TRY(placement_init(p));
	TRY(placement_thread_start(p));
	TRY(placement_event_start(p));
	return 0;
}

static void exit(void)
{
	struct placement *p = &__global_placement;
	placement_drop(p);
}

module_init(init);
module_exit(exit);
MODULE_AUTHOR("Junliang Hu <jlhu@cse.cuhk.edu.hk>");
MODULE_DESCRIPTION("Memory placement optimization module");
MODULE_LICENSE("GPL");
