#include "spsc.h"
#include <linux/cpumask.h>
#include <linux/printk.h>
#include <linux/perf_event.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "hagent.h"
#include "module.h"
#include "sdh.h"
#include "pebs.h"

static unsigned long task_vsize(struct mm_struct *mm)
{
	return PAGE_SIZE * mm->total_vm;
}

static void hagent_drain_pebs_work_func(struct irq_work *work)
{
	struct hagent_info *info =
		container_of(work, struct hagent_info, identification);
	hagent_drain_pebs(info);
}

static struct hagent_info *hagent_info_new(pid_t tgid)
{
	pr_info("creating hagent_info");
	struct task_struct *task = get_pid_task(find_vpid(tgid), PIDTYPE_PID);
	BUG_ON(!task);
	struct hagent_info *info =
		kvzalloc(sizeof(struct hagent_info), GFP_KERNEL);
	BUG_ON(!info);
	pr_cont(" %px for process %d", info, tgid);
	struct hagent_info init = {
		.tgid = tgid,
		.task = task,
		.vsize = task_vsize(task->mm),
		.mm = task->mm,
		.sdh = sdh_new(hagent_sdh_w, hagent_sdh_d, hagent_sdh_k),
	};
	BUG_ON(!init.sdh);
	*info = init;
	INIT_LIST_HEAD(&info->link);
	return info;
}

static void hagent_info_drop(struct hagent_info *info)
{
	pr_info("deleting hagent_info %px for process %d", info, info->tgid);
	// BUG_ON(info->event);
	kvfree(info->sample_chan);
	sdh_drop(info->sdh);
	kvfree(info);
}

struct hagent {
	struct mutex lock;
	// The dominating process currently managed by hagent.
	struct hagent_info *currently_managed;
	// All allocated hagent_info struct
	struct list_head hagent_info_list;
};

static struct hagent *hagent_new(void)
{
	struct hagent *hagent = kvzalloc(sizeof(struct hagent), GFP_KERNEL);
	BUG_ON(!hagent);
	mutex_init(&hagent->lock);
	INIT_LIST_HEAD(&hagent->hagent_info_list);
	return hagent;
}

static void hagent_release_event(struct hagent_info *info)
{
	for (int cpu = 0; cpu < num_online_cpus() && info->event[cpu]; ++cpu) {
		pr_info("%s: releasing kernel event for process %d cpu %d",
			__func__, info->tgid, cpu);
		struct hagent_info_event_args *args =
			info->event[cpu]->overflow_handler_context;
		BUG_ON(perf_event_release_kernel(info->event[cpu]));
		spsc_drop(info->sample_chan[cpu]);
		hagent_info_event_args_drop(args);
		info->event[cpu] = NULL;
		info->sample_chan[cpu] = NULL;
	}

	if (hagent_dump_topk) {
		sdh_show_topk(info->sdh);
	}
	// no futher samples will be generated, safe to cancel here
	irq_work_sync(&info->identification);
}

static void hagent_release_current(struct hagent *hagent)
{
	struct hagent_info **info = &hagent->currently_managed;
	if (*info) {
		hagent_release_event(*info);
		*info = NULL;
	}
}

static void hagent_drop(struct hagent *hagent)
{
	BUG_ON(!hagent);

	struct hagent_info *info;
	list_for_each_entry(info, &hagent->hagent_info_list, link) {
		hagent_release_event(info);
		hagent_info_drop(info);
	}
	kvfree(hagent);
}

// only one signleton
static struct hagent *__instance;

int __init hagent_init(void)
{
	__instance = hagent_new();
	return 0;
}

void __exit hagent_exit(void)
{
	hagent_drop(__instance);
}

static struct hagent *hagent_lock(void)
{
	mutex_lock(&__instance->lock);
	return __instance;
}

static void hagent_unlock(struct hagent *hagent)
{
	mutex_unlock(&hagent->lock);
}

static struct hagent_info *hagent_find(struct hagent *hagent, pid_t tgid)
{
	if (!hagent->currently_managed) {
		return NULL;
	}
	if (tgid == hagent->currently_managed->tgid) {
		return hagent->currently_managed;
	}
	struct hagent_info *info;
	list_for_each_entry(info, &hagent->hagent_info_list, link) {
		if (info->tgid == tgid) {
			return info;
		}
	}
	return NULL;
}

static void hagent_insert(struct hagent *hagent, struct hagent_info *info)
{
	list_add_tail(&info->link, &hagent->hagent_info_list);
}

static void hagent_remove(struct hagent *hagent, struct hagent_info *info)
{
	list_del(&info->link);
}

// we only track the process that has a vsize larger than 1/3 of the total
// possible memory size (DRAM + PMEM)
static bool hagent_should_track(pid_t tgid, unsigned long new_len)
{
	struct mm_struct *mm = get_pid_task(find_vpid(tgid), PIDTYPE_PID)->mm;
	if (((task_vsize(mm) + new_len) >> PAGE_SHIFT) * 3 > totalram_pages()) {
		return true;
	}
	return false;
}

extern u64 perf_virt_to_phys(u64 virt);

// pebs might be drained when scheduling or in NMI handler, see:
//  - sched: prepare_task_switch()
//             => perf_event_task_sched_out()
//               => perf_pmu_sched_task()
//                 => (pmu->sched_task)()
//                   => x86_pmu_sched_task()
//		       => intel_pmu_sched_task()
//		         => intel_pmu_pebs_sched_task()
//		           => intel_pmu_drain_pebs_buffer()
//		             => intel_pmu_drain_pebs_icl()
//		               => __intel_pmu_pebs_event()
//               		 => (event->overflow_handler)()
//  - NMI: perf_event_nmi_handler()
//           => x86_pmu_handle_irq()
//             => perf_event_overflow()
//		 => (event->overflow_handler)()
inline static void hagent_drain_pebs_handler(struct perf_event *event,
					     struct perf_sample_data *data,
					     struct pt_regs *regs)
{
	static volatile unsigned long sched_count = 0;
	struct hagent_info_event_args *args = event->overflow_handler_context;
	struct hagent_info *info = args->info;
	int cpu = args->cpu;
	struct hagent_sample sample = {
		.nth = ++sched_count,
		.va = data->addr & HPAGE_MASK,
		// .va = data->addr,
		.latency = data->weight.full,
		.pa = perf_virt_to_phys(data->addr & HPAGE_MASK),
		// .pa = perf_virt_to_phys(data->addr),
		// .pid = data->tid_entry.pid,
		// .tid = data->tid_entry.tid,
	};
	spsc_send(info->sample_chan[cpu], &sample);

	// when sent 1/64 of the total channel capacity, trigger the other side
	// if we happens to be in NMI, queue_work() cannot be used due to
	// ~~locking contrains~~, it's OK we wait for the next try.
	// Actually, it's because a work item cannot be queued multiple times.
	if (!in_nmi() && sample.nth % (hagent_channel_capacity >> 6) == 0) {
		irq_work_queue_on(&info->identification, 0);
		// irq_work_queue(&info->identification);
	}
}

// switch the currently tracking process to the provided one
static void hagent_switch(struct hagent *hagent, struct hagent_info *info)
{
	hagent_release_current(hagent);

	struct perf_event_attr attr = {
		.type = PERF_TYPE_RAW,
		.size = sizeof(struct perf_event_attr),
		.config = hagent_event_config,
		.config1 = hagent_event_threshold,
		.sample_type = PERF_SAMPLE_ADDR | PERF_SAMPLE_WEIGHT |
			       PERF_SAMPLE_TID, // | PERF_SAMPLE_PHYS_ADDR,
		.sample_period = hagent_event_period,
		.precise_ip = 3,
		.disabled = 0,
		.exclude_kernel = 1,
		.exclude_callchain_kernel = 1,
	};
	pr_info("creating kernel event for process %d config 0x%llx confgi1 0x%llx sample_type 0x%llx sample_period 0x%llx precise_ip %d",
		info->tgid, attr.config, attr.config1, attr.sample_type,
		attr.sample_period, attr.precise_ip);

	// info->event = perf_event_create_kernel_counter(
	// 	&attr, -1, info->task, hagent_drain_pebs_handler, info);
	init_irq_work(&info->identification, hagent_drain_pebs_work_func);
	for (int cpu = 0; cpu < num_online_cpus(); ++cpu) {
		struct hagent_info_event_args *args =
			hagent_info_event_args_new(cpu, info);
		info->sample_chan[cpu] = spsc_new(hagent_channel_capacity);
		info->event[cpu] = perf_event_create_kernel_counter(
			&attr, cpu, NULL, hagent_drain_pebs_handler, args);
		BUG_ON(!info->event[cpu]);
	}
	hagent->currently_managed = info;
}

void hagent_callback_mmap(pid_t tgid, unsigned long addr, unsigned long len,
			  unsigned long mapped)
{
	if (len > PUD_SIZE) {
		pr_info("%s(tgid=%d,addr=%px,len=0x%lx,mapped=%px)", __func__,
			tgid, (void *)addr, len, (void *)mapped);
	}
	struct hagent *hagent = hagent_lock();
	struct hagent_info *info = hagent_find(hagent, tgid);
	if (!info) {
		if (!hagent_should_track(tgid, len)) {
			goto unlock;
		}
		info = hagent_info_new(tgid);
		hagent_insert(hagent, info);
	}
	// the purpose of this callback: update vsize and check if we should
	// switch tracking to another process
	info->vsize = task_vsize(info->mm);
	// FIXME: maybe we should track the largest mmap region too
	if (!hagent->currently_managed) {
		hagent_switch(hagent, info);
	}
unlock:
	hagent_unlock(hagent);
}

// we hook exit_group because it's only called once when all threads exit
void hagent_callback_exit_group(pid_t tgid)
{
	pr_debug("%s(tgid=%d)", __func__, tgid);
	struct hagent *hagent = hagent_lock();
	struct hagent_info *info = hagent_find(hagent, tgid);
	if (info) {
		if (hagent->currently_managed == info) {
			hagent_release_current(hagent);
		}
		hagent_remove(hagent, info);
		hagent_info_drop(info);
	}
	hagent_unlock(hagent);
}
