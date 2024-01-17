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

struct task_struct *access_sampling = NULL;
struct perf_event ***mem_event;

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
	mem_event[cpu][type] = fget(event_fd)->private_data;
	return 0;
}

static int pebs_init(pid_t pid, int node)
{
	int cpu, event;

	mem_event = kzalloc(sizeof(struct perf_event **) * CPUS_PER_SOCKET,
			    GFP_KERNEL);
	for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
		mem_event[cpu] = kzalloc(
			sizeof(struct perf_event *) * N_HTMMEVENTS, GFP_KERNEL);
	}

	printk("pebs_init\n");
	for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
		for (event = 0; event < N_HTMMEVENTS; event++) {
			if (get_pebs_event(event) == N_HTMMEVENTS) {
				mem_event[cpu][event] = NULL;
				continue;
			}

			if (__perf_event_open(get_pebs_event(event), 0, cpu,
					      event, pid))
				return -1;
			if (htmm__perf_event_init(mem_event[cpu][event],
						  BUFFER_SIZE))
				return -1;
		}
	}

	return 0;
}

static void pebs_disable(void)
{
	int cpu, event;

	printk("pebs disable\n");
	for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
		for (event = 0; event < N_HTMMEVENTS; event++) {
			if (mem_event[cpu][event])
				perf_event_disable(mem_event[cpu][event]);
		}
	}
}

static void pebs_enable(void)
{
	int cpu, event;

	printk("pebs enable\n");
	for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
		for (event = 0; event < N_HTMMEVENTS; event++) {
			if (mem_event[cpu][event])
				perf_event_enable(mem_event[cpu][event]);
		}
	}
}

static void pebs_update_period(uint64_t value, uint64_t inst_value)
{
	int cpu, event;

	for (cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
		for (event = 0; event < N_HTMMEVENTS; event++) {
			int ret;
			if (!mem_event[cpu][event])
				continue;

			switch (event) {
			case DRAMREAD:
			case NVMREAD:
			case CXLREAD:
				ret = perf_event_period(mem_event[cpu][event],
							value);
				break;
			case MEMWRITE:
				ret = perf_event_period(mem_event[cpu][event],
							inst_value);
				break;
			default:
				ret = 0;
				break;
			}

			if (ret == -EINVAL)
				printk("failed to update sample period");
		}
	}
}

struct ksamplingd_measurements {
	unsigned long long nr_sampled, nr_dram, nr_nvm, nr_write;
	unsigned long long nr_throttled, nr_lost, nr_unknown;
	unsigned long long nr_skip;
	/* for analytic purpose */
	unsigned long hr_dram, hr_nvm;
};

static int ksamplingd_iter(int cpu, int event,
			   struct ksamplingd_measurements *measurements)
{
	u64 begin = local_clock();
	struct perf_buffer *rb;
	struct perf_event_mmap_page *up;
	struct perf_event_header *ph;
	struct htmm_event *he;
	unsigned long pg_index, offset;
	int page_shift;
	int ret = -EAGAIN;
	__u64 head;

	if (!mem_event[cpu][event]) {
		return -ENOENT;
	}

	__sync_synchronize();

	rb = mem_event[cpu][event]->rb;
	if (!rb) {
		return -ENOENT;
	}
	/* perf_buffer is ring buffer */
	up = READ_ONCE(rb->user_page);
	head = READ_ONCE(up->data_head);
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

	page_shift = PAGE_SHIFT + page_order(rb);
	/* get address of a tail sample */
	offset = READ_ONCE(up->data_tail);
	pg_index = (offset >> page_shift) & (rb->nr_pages - 1);
	offset &= (1 << page_shift) - 1;

	ph = (void *)(rb->data_pages[pg_index] + offset);
	switch (ph->type) {
	case PERF_RECORD_SAMPLE:
		he = (struct htmm_event *)ph;
		if (!valid_va(he->addr)) {
			break;
		}

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

static void ksamplingd_throttle(void)
{
}

static int ksamplingd(void *data)
{
	pr_info("%s: started\n", __func__);

	struct ksamplingd_measurements measurements = {};

	/* used for calculating average cpu usage of ksampled */
	struct task_struct *t = current;
	/* a unit of cputime: permil (1/1000) */
	u64 total_runtime, exec_runtime, cputime = 0;
	unsigned long total_cputime, elapsed_cputime, cur;
	/* used for periodic checks*/
	unsigned long cpucap_period = msecs_to_jiffies(15000); // 15s
	unsigned long sample_period = 0;
	unsigned long sample_inst_period = 0;
	/* report cpu/period stat */
	unsigned long trace_cputime,
		trace_period = msecs_to_jiffies(1500); // 3s
	unsigned long trace_runtime;

	/* orig impl: see read_sum_exec_runtime() */
	trace_runtime = total_runtime = exec_runtime = t->se.sum_exec_runtime;

	trace_cputime = total_cputime = elapsed_cputime = jiffies;

	/* TODO implements per-CPU node ksamplingd by using pg_data_t */
	/* Currently uses a single CPU node(0) */
	// const struct cpumask *cpumask = cpumask_of_node(0);
	// if (!cpumask_empty(cpumask)) {
	// 	do_set_cpus_allowed(access_sampling, cpumask);
	// }

	while (!kthread_should_stop()) {
		if (htmm_mode == HTMM_NO_MIG) {
			msleep_interruptible(10000);
			continue;
		}

		for (int cpu = 0; cpu < CPUS_PER_SOCKET; cpu++) {
			for (int event = 0; event < N_HTMMEVENTS; event++) {
				// clang-format off
				while (-EAGAIN == ksamplingd_iter(cpu, event,&measurements));
				// clang-format on
			}
		}
		/* if ksampled_soft_cpu_quota is zero, disable dynamic pebs feature */
		if (!ksampled_soft_cpu_quota)
			continue;

		/* sleep */
		schedule_timeout_interruptible(usecs_to_jiffies(2000));

		u64 begin = local_clock();
		/* check elasped time */
		cur = jiffies;
		if ((cur - elapsed_cputime) >= cpucap_period) {
			u64 cur_runtime = t->se.sum_exec_runtime;
			exec_runtime = cur_runtime - exec_runtime; //ns
			elapsed_cputime =
				jiffies_to_usecs(cur - elapsed_cputime); //us
			if (!cputime) {
				u64 cur_cputime =
					exec_runtime / elapsed_cputime;
				// EMA with the scale factor (0.2)
				cputime = (cur_cputime << 3) + (cputime << 1);
				cputime /= 10;
			} else {
				cputime = exec_runtime / elapsed_cputime;
			}

			/* to prevent frequent updates, allow for a slight variation of +/- 0.5% */
			if (cputime > (ksampled_soft_cpu_quota + 5) &&
			    sample_period != pcount) {
				/* need to increase the sample period */
				/* only increase by 1 */
				unsigned long tmp1 = sample_period,
					      tmp2 = sample_inst_period;
				increase_sample_period(&sample_period,
						       &sample_inst_period);
				if (tmp1 != sample_period ||
				    tmp2 != sample_inst_period) {
					pebs_update_period(
						get_sample_period(
							sample_period),
						get_sample_inst_period(
							sample_inst_period));
				}
			} else if (cputime < (ksampled_soft_cpu_quota - 5) &&
				   sample_period) {
				unsigned long tmp1 = sample_period,
					      tmp2 = sample_inst_period;
				decrease_sample_period(&sample_period,
						       &sample_inst_period);
				if (tmp1 != sample_period ||
				    tmp2 != sample_inst_period) {
					pebs_update_period(
						get_sample_period(
							sample_period),
						get_sample_inst_period(
							sample_inst_period));
				}
			}
			/* does it need to prevent ping-pong behavior? */

			elapsed_cputime = cur;
			exec_runtime = cur_runtime;
		}

		/* This is used for reporting the sample period and cputime */
		if (cur - trace_cputime >= trace_period) {
			unsigned long hr = 0;
			u64 cur_runtime = t->se.sum_exec_runtime;
			trace_runtime = cur_runtime - trace_runtime;
			trace_cputime = jiffies_to_usecs(cur - trace_cputime);
			trace_cputime = trace_runtime / trace_cputime;

			if (measurements.hr_dram + measurements.hr_nvm == 0) {
				hr = 0;
			} else {
				hr = measurements.hr_dram * 10000;
				hr /= measurements.hr_dram +
				      measurements.hr_nvm;
			}
			trace_printk(
				"sample_period: %lu || cputime: %lu  || hit ratio: %lu\n",
				get_sample_period(sample_period), trace_cputime,
				hr);

			measurements.hr_dram = measurements.hr_nvm = 0;
			trace_cputime = cur;
			trace_runtime = cur_runtime;
		}
		count_vm_events(PEBS_COLLECTION_COST, local_clock() - begin);
	}

	total_runtime = (t->se.sum_exec_runtime) - total_runtime; // ns
	total_cputime = jiffies_to_usecs(jiffies - total_cputime); // us

	printk("nr_sampled: %llu, nr_throttled: %llu, nr_lost: %llu\n",
	       measurements.nr_sampled, measurements.nr_throttled,
	       measurements.nr_lost);
	printk("total runtime: %llu ns, total cputime: %lu us, cpu usage: %llu\n",
	       total_runtime, total_cputime, (total_runtime) / total_cputime);

	return 0;
}

static int ksamplingd_run(void)
{
	int err = 0;

	if (!access_sampling) {
		access_sampling = kthread_run(ksamplingd, NULL, "ksamplingd");
		if (IS_ERR(access_sampling)) {
			err = PTR_ERR(access_sampling);
			access_sampling = NULL;
		}
	}
	return err;
}

int ksamplingd_init(pid_t pid, int node)
{
	int ret;

	if (access_sampling)
		return 0;

	ret = pebs_init(pid, node);
	if (ret) {
		printk("htmm__perf_event_init failure... ERROR:%d\n", ret);
		return 0;
	}

	return ksamplingd_run();
}

void ksamplingd_exit(void)
{
	if (access_sampling) {
		kthread_stop(access_sampling);
		access_sampling = NULL;
	}
	pebs_disable();
}
