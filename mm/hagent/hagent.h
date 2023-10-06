#ifndef __MM_HAGENT_H
#define __MM_HAGENT_H

#include <linux/irq_work.h>
#include <linux/mutex.h>
#include <linux/types.h>

void hagent_callback_mmap(pid_t tgid, unsigned long address, unsigned long len,
			  unsigned long mapped);
void hagent_callback_exit_group(pid_t tgid);

int __init hagent_init(void);
void __exit hagent_exit(void);

struct sdh;
// The hagent only tracks the virtual memory usage of the dominating process.
// To ensure we are always managing the dominating process, we always keep its
// pid and vsz.
// When finding out a new dominating process, we will allocate managing
// structure for that process and stop the perf_event on old process.
// But the old managing structure will not be freed until that process exits.
// This is to ensure no jumping between processes happens, which might causing
// constantly allocating and free resources.
//
// To implement this, we need to hook `mmap` returns and `exit` enters.
// exit does not distinguish between threads and processes,
// we need to check every task_struct in a thread group (process).
struct hagent_info {
	// All allocated dominating_process_info are linked together.
	struct list_head link;

	// In kernel, we use tgid to identify a process (thread group).
	// Kernel's pid is equivalent to userspace tid, and kernel's tgid is equivalent
	// to userspace pid.
	pid_t tgid;
	struct task_struct *task;

	// The total virtual memory size. See `task_vsize()` in `fs/proc/task_mmu.c`.
	unsigned long vsize;

	// Only valid when the process is indeed a dominating process.
	// Created via `perf_event_create_kernel_counter()` when identified.
	// Released via `perf_event_release_kernel()` when new one is identified.
	struct perf_event *event[NR_CPUS];
	// SPSC channel for offload hotness identification to the workqueue
	// asynchronously
	struct spsc *sample_chan[NR_CPUS];

	// The address space of the dominating process
	struct mm_struct *mm;

	struct sdh *sdh;

	struct irq_work identification;
};

struct hagent_info_event_args {
	int cpu;
	struct hagent_info *info;
};

#include "spsc.h"

inline static struct hagent_info_event_args *
hagent_info_event_args_new(int cpu, struct hagent_info *info)
{
	struct hagent_info_event_args init = {
		.cpu = cpu, .info = info,
	}, *new = kvzalloc(sizeof(struct hagent_info_event_args ), GFP_KERNEL);
	BUG_ON(!new);
	*new = init;
	return new;
}

inline static void
hagent_info_event_args_drop(struct hagent_info_event_args *args)
{
	if (args) {
		kvfree(args);
	}
}

#endif
