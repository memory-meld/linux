/*
 * memory access sampling for hugepage-aware tiered memory management.
 */
#include <linux/kthread.h>
#include <linux/memcontrol.h>
#include <linux/mempolicy.h>
#include <linux/sched.h>
#include <linux/perf_event.h>
#include <linux/delay.h>
#include <linux/sched/cputime.h>

#include "../kernel/events/internal.h"

#include <linux/htmm.h>

struct ksamplingd_measurements {
	unsigned long long nr_sampled, nr_dram, nr_nvm, nr_write;
	unsigned long long nr_throttled, nr_lost, nr_unknown;
	unsigned long long nr_skip;
	/* for analytic purpose */
	unsigned long hr_dram, hr_nvm;
};

struct ksamplingd {
	struct task_struct *t;
	struct perf_event ***mem_event;
	ulong llc_idx, inst_idx;
	struct ksamplingd_measurements measurements;
};

struct ksamplingd *__ksamplingd_instance = NULL;

// struct task_struct *access_sampling = NULL;
// struct perf_event ***mem_event;
// ulong llc_idx, inst_idx;

static bool valid_va(unsigned long addr)
{
	if (!(addr >> (PGDIR_SHIFT + 9)) && addr != 0)
		return true;
	else
		return false;
}

static __u64 get_pebs_event(enum events e)
{
	switch (e) {
	case DRAMREAD:
		return DRAM_LLC_LOAD_MISS;
	case NVMREAD:
		if (!htmm_cxl_mode)
			return NVM_LLC_LOAD_MISS;
		else
			return N_HTMMEVENTS;
	case MEMWRITE:
		return ALL_STORES;
	case CXLREAD:
		if (htmm_cxl_mode)
			return REMOTE_DRAM_LLC_LOAD_MISS;
		else
			return N_HTMMEVENTS;
	default:
		return N_HTMMEVENTS;
	}
}

static int __perf_event_open(__u64 config, __u64 config1, __u64 cpu, __u64 type,
			     __u32 pid)
{
	struct perf_event_attr attr;
	struct file *file;
	int event_fd, __pid;

	memset(&attr, 0, sizeof(struct perf_event_attr));

	attr.type = PERF_TYPE_RAW;
	attr.size = sizeof(struct perf_event_attr);
	attr.config = config;
	attr.config1 = config1;
	if (config == ALL_STORES)
		attr.sample_period = htmm_inst_sample_period;
	else
		attr.sample_period = get_sample_period(0);
	attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR;
	attr.disabled = 0;
	// must set, or no sample will be collected when using the preload helper
	attr.inherit = 1;
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;
	attr.exclude_callchain_kernel = 1;
	attr.exclude_callchain_user = 1;
	attr.precise_ip = 3;
	// attr.enable_on_exec = 1;

	if (pid == 0)
		__pid = -1;
	else
		__pid = pid;

	event_fd = kernel_perf_event_open(&attr, __pid, cpu, -1, 0);
	pr_info("%s: perf_event_open(pid=%d, cpu=%d, config=0x%lx, config1=0x%lx, type=0x%lx) = event_fd=%d\n",
		__func__, __pid, cpu, config, config1, attr.sample_type,
		event_fd);
	if (event_fd <= 0) {
		return -1;
	}

	file = fget(event_fd);
	if (!file) {
		pr_err("%s: invalid file\n", __func__);
		return -1;
	}
	return event_fd;
}

static int pebs_init(struct ksamplingd *ksamplingd, pid_t pid, int node)
{
	ksamplingd->mem_event = kzalloc(
		sizeof(struct perf_event **) * CPUS_PER_SOCKET, GFP_KERNEL);
	for (int cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
		ksamplingd->mem_event[cpu] = kzalloc(
			sizeof(struct perf_event *) * N_HTMMEVENTS, GFP_KERNEL);
	}

	printk("pebs_init\n");
	for (int cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
		for (int event = 0, event_fd; event < N_HTMMEVENTS; event++) {
			if (get_pebs_event(event) == N_HTMMEVENTS) {
				ksamplingd->mem_event[cpu][event] = NULL;
				continue;
			}
			event_fd = __perf_event_open(get_pebs_event(event), 0,
						     cpu, event, pid);
			if (event_fd < 0)
				return -1;
			ksamplingd->mem_event[cpu][event] =
				fget(event_fd)->private_data;
			if (htmm__perf_event_init(
				    ksamplingd->mem_event[cpu][event],
				    BUFFER_SIZE))
				return -1;
		}
	}

	return 0;
}

static void pebs_disable(struct ksamplingd *ksamplingd)
{
	int cpu, event;

	printk("pebs disable\n");
	for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
		for (event = 0; event < N_HTMMEVENTS; event++) {
			struct perf_event *e =
				ksamplingd->mem_event[cpu][event];
			if (e)
				perf_event_disable(e);
		}
	}
}

static void pebs_enable(struct ksamplingd *ksamplingd)
{
	int cpu, event;

	printk("pebs enable\n");
	for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
		for (event = 0; event < N_HTMMEVENTS; event++) {
			struct perf_event *e =
				ksamplingd->mem_event[cpu][event];
			if (e)
				perf_event_enable(e);
		}
	}
}

static void pebs_update_period(struct ksamplingd *ksamplingd)
{
	u64 llc_perido = get_sample_period(ksamplingd->llc_idx);
	u64 inst_period = get_sample_inst_period(ksamplingd->inst_idx);
	pr_info_ratelimited("%s: llc_perido: %llu, inst_period: %llu\n",
			    __func__, llc_perido, inst_period);

	for (int cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
		for (int event = 0; event < N_HTMMEVENTS; event++) {
			struct perf_event *e =
				ksamplingd->mem_event[cpu][event];
			int ret = 0;
			if (!e)
				continue;
			switch (event) {
			case DRAMREAD:
			case NVMREAD:
			case CXLREAD:
				ret = perf_event_period(e, llc_perido);
				break;
			case MEMWRITE:
				ret = perf_event_period(e, inst_period);
				break;
			default:
				ret = 0;
				break;
			}

			if (ret < 0)
				printk("failed to update sample period");
		}
	}
}

static int ksamplingd_iter(struct ksamplingd *ksamplingd, int cpu, int event)
{
	struct ksamplingd_measurements *measurements =
		&ksamplingd->measurements;
	u64 begin = local_clock();
	int ret = -EAGAIN;

	struct perf_event *e = ksamplingd->mem_event[cpu][event];

	if (!e) {
		return -ENOENT;
	}

	__sync_synchronize();

	struct perf_buffer *rb = e->rb;
	if (!rb) {
		return -ENOENT;
	}
	/* perf_buffer is ring buffer */
	struct perf_event_mmap_page *up = READ_ONCE(rb->user_page);
	u64 head = READ_ONCE(up->data_head);
	if (head == up->data_tail) {
		if (cpu < 16) {
			measurements->nr_skip++;
		}
		return -ENOSPC;
	}

	head -= up->data_tail;
	if (head > (BUFFER_SIZE * ksampled_max_sample_ratio / 100)) {
		ret = -EAGAIN;
	} else if (head < (BUFFER_SIZE * ksampled_min_sample_ratio / 100)) {
		ret = -EBUSY;
	}

	/* read barrier */
	smp_rmb();

	unsigned long page_shift = PAGE_SHIFT + page_order(rb);
	/* get address of a tail sample */
	unsigned long offset = READ_ONCE(up->data_tail);
	unsigned long pg_index = (offset >> page_shift) & (rb->nr_pages - 1);
	offset &= (1 << page_shift) - 1;

	struct perf_event_header *ph =
		(void *)(rb->data_pages[pg_index] + offset);
	struct htmm_event *he = (struct htmm_event *)ph;
	switch (ph->type) {
	case PERF_RECORD_SAMPLE:
		if (!valid_va(he->addr)) {
			break;
		}

		count_vm_event(PEBS_SAMPLE_COLLECTED);
		count_vm_events(PEBS_COLLECTION_COST, local_clock() - begin);
		scoped_guard(vmevent, HOTNESS_IDENTIFICATION_COST)
		{
			update_pginfo(he->pid, he->addr, event);
		}
		begin = local_clock();

		//count_vm_event(HTMM_NR_SAMPLED);
		measurements->nr_sampled++;

		if (event == DRAMREAD) {
			measurements->nr_dram++;
			measurements->hr_dram++;
		} else if (event == CXLREAD || event == NVMREAD) {
			measurements->nr_nvm++;
			measurements->hr_nvm++;
		} else {
			measurements->nr_write++;
		}
		break;
	case PERF_RECORD_THROTTLE:
	case PERF_RECORD_UNTHROTTLE:
		measurements->nr_throttled++;
		break;
	case PERF_RECORD_LOST_SAMPLES:
		measurements->nr_lost++;
		break;
	default:
		measurements->nr_unknown++;
		break;
	}
	if (measurements->nr_sampled % 500000 == 0) {
		trace_printk(
			"nr_sampled: %llu, nr_dram: %llu, nr_nvm: %llu, nr_write: %llu, nr_throttled: %llu \n",
			measurements->nr_sampled, measurements->nr_dram,
			measurements->nr_nvm, measurements->nr_write,
			measurements->nr_throttled);
		measurements->nr_dram = 0;
		measurements->nr_nvm = 0;
		measurements->nr_write = 0;
	}
	/* read, write barrier */
	smp_mb();
	WRITE_ONCE(up->data_tail, up->data_tail + ph->size);
	count_vm_events(PEBS_COLLECTION_COST, local_clock() - begin);
	return ret;
}

struct ksamplingd_time {
	// in unit of jiffy (1/HZ)
	u64 cputime;
	// in unit of ns (scheduler clock)
	u64 runtime;
};

static void ksamplingd_throttle_pebs(struct ksamplingd *ksamplingd,
				     struct ksamplingd_time *last,
				     u64 *usage_ema_x1000)
{
	struct task_struct *t = ksamplingd->t;
	ulong *llc_idx = &ksamplingd->llc_idx,
	      *inst_idx = &ksamplingd->inst_idx;
	struct ksamplingd_time now = {
		.cputime = jiffies,
		.runtime = t->se.sum_exec_runtime,
	};
	u64 cputime_delta = now.cputime - last->cputime;
	// cpucap_period = 15s
	if (cputime_delta >= msecs_to_jiffies(15000)) {
		u64 runtime_delta_ns = now.runtime - last->runtime;
		u64 cputime_delta_us = jiffies_to_usecs(cputime_delta);
		if (!*usage_ema_x1000) {
			u64 usage_x1000 = runtime_delta_ns / cputime_delta_us;
			// Exponential moving average with a scale factor of 0.2
			*usage_ema_x1000 =
				(usage_x1000 << 3) + (*usage_ema_x1000 << 1);
			*usage_ema_x1000 /= 10;
		} else {
			*usage_ema_x1000 = runtime_delta_ns / cputime_delta_us;
		}

		/* to prevent frequent updates, allow for a slight variation of +/- 0.5% */
		if (*usage_ema_x1000 > ksampled_soft_cpu_quota + 5 &&
		    increase_sample_period(llc_idx, inst_idx)) {
			pebs_update_period(ksamplingd);
		}
		if (*usage_ema_x1000 < ksampled_soft_cpu_quota - 5 &&
		    decrease_sample_period(llc_idx, inst_idx)) {
			pebs_update_period(ksamplingd);
		}
		/* does it need to prevent ping-pong behavior? */
		*last = now;
	}
}

static void ksamplingd_throttle_report(struct ksamplingd *ksamplingd,
				       struct ksamplingd_time *last)
{
	/* This is used for reporting the sample period and cputime */
	struct task_struct *t = ksamplingd->t;
	struct ksamplingd_measurements *measurements =
		&ksamplingd->measurements;
	struct ksamplingd_time now = {
		.cputime = jiffies,
		.runtime = t->se.sum_exec_runtime,
	};
	u64 cputime_delta = now.cputime - last->cputime;
	// trace_period = 3s
	if (cputime_delta >= msecs_to_jiffies(1500)) {
		u64 runtime_delta_ns = now.runtime - last->runtime;
		u64 usage_x1000 =
			runtime_delta_ns / jiffies_to_usecs(cputime_delta);
		u64 hit_rate_x1000 = measurements->hr_dram * 10000;
		hit_rate_x1000 /=
			1 + measurements->hr_dram + measurements->hr_nvm;
		trace_printk(
			"sample_period: %lu || cpu usage: %lu  || hit rate: %lu\n",
			get_sample_period(ksamplingd->llc_idx), usage_x1000,
			hit_rate_x1000);
		measurements->hr_dram = measurements->hr_nvm = 0;
		*last = now;
	}
}

static int ksamplingd_fn(struct ksamplingd *ksamplingd)
{
	msleep(10);
	pr_info("%s: started\n", __func__);
	struct ksamplingd_measurements *measurements =
		&ksamplingd->measurements;

	/* used for calculating average cpu usage of ksampled */
	struct task_struct *t = current;
	/* a unit of cputime: permil (1/1000) */
	u64 usage_ema_x1000 = 0;
	/* used for periodic checks*/

	struct ksamplingd_time pebs_last, report_last, ksamplingd_begin;
	pebs_last = report_last = ksamplingd_begin = (struct ksamplingd_time){
		.cputime = jiffies,
		/* orig impl: see read_sum_exec_runtime() */
		.runtime = t->se.sum_exec_runtime,
	};
	u64 iterations = 0;

	/* TODO implements per-CPU node ksamplingd by using pg_data_t */
	/* Currently uses a single CPU node(0) */
	// const struct cpumask *cpumask = cpumask_of_node(0);
	// if (!cpumask_empty(cpumask)) {
	// 	do_set_cpus_allowed(access_sampling, cpumask);
	// }

	while (!kthread_should_stop()) {
		iterations += 1;
		if (htmm_mode == HTMM_NO_MIG) {
			msleep_interruptible(10000);
			continue;
		}

		for (int cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
			for (int event = 0; event < N_HTMMEVENTS; event++) {
				// clang-format off
				while (-EAGAIN == ksamplingd_iter(ksamplingd, cpu, event));
				// clang-format on
			}
		}
		/* if ksampled_soft_cpu_quota is zero, disable dynamic pebs feature */
		if (!ksampled_soft_cpu_quota)
			continue;

		/* sleep */
		schedule_timeout_interruptible(usecs_to_jiffies(2000));

		/* check elasped time */
		ksamplingd_throttle_pebs(ksamplingd, &pebs_last,
					 &usage_ema_x1000);
		ksamplingd_throttle_report(ksamplingd, &report_last);
	}

	struct ksamplingd_time total = {
		// us
		.cputime = jiffies_to_usecs(jiffies - ksamplingd_begin.cputime),
		// ns
		.runtime = t->se.sum_exec_runtime - ksamplingd_begin.runtime,
	};
	printk("%s: nr_sampled: %llu, nr_throttled: %llu, nr_lost: %llu\n",
	       __func__, measurements->nr_sampled, measurements->nr_throttled,
	       measurements->nr_lost);
	printk("%s: total runtime: %llu ns, total cputime: %lu us, cpu usage: %llu, iterations: %llu\n",
	       __func__, total.runtime, total.cputime,
	       total.runtime / (1 + total.cputime), iterations);

	return 0;
}

static int ksamplingd_run(struct ksamplingd *ksamplingd)
{
	if (ksamplingd->t) {
		return -EINVAL;
	}
	struct task_struct *t = kthread_run((int (*)(void *))ksamplingd_fn,
					    ksamplingd, "ksamplingd");
	int err = 0;
	if (IS_ERR(t)) {
		err = PTR_ERR(t);
		t = NULL;
	}
	ksamplingd->t = t;

	return err;
}

int ksamplingd_init(pid_t pid, int node)
{
	int ret;

	if (__ksamplingd_instance)
		return 0;
	__ksamplingd_instance = kvzalloc(sizeof(struct ksamplingd), GFP_KERNEL);
	ret = pebs_init(__ksamplingd_instance, pid, node);
	if (ret) {
		printk("htmm__perf_event_init failure... ERROR:%d\n", ret);
		return 0;
	}

	return ksamplingd_run(__ksamplingd_instance);
}

void ksamplingd_exit()
{
	struct ksamplingd *d = __ksamplingd_instance;
	if (d) {
		if (d->t) {
			kthread_stop(d->t);
			d->t = NULL;
		}
		pebs_disable(d);
		kvfree(d);
	}
	__ksamplingd_instance = NULL;
}
