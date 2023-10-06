#ifndef __MM_HAGENT_PEBS_H
#define __MM_HAGENT_PEBS_H

#include <linux/perf_event.h>

struct hagent_sample {
	unsigned long nth, va, latency, pa;
	// unsigned pid, tid;
};

struct hagent_info;
void hagent_drain_pebs(struct hagent_info *info);

#endif
