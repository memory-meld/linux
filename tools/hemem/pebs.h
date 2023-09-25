#ifndef HEMEM_PEBS_H
#define HEMEM_PEBS_H

#include <linux/perf_event.h>

#include "envconst.h"

struct perf_sample {
	struct perf_event_header header;
	__u64 ip;
	__u32 pid, tid; /* if PERF_SAMPLE_TID */
	__u64 addr; /* if PERF_SAMPLE_ADDR */
	__u64 weight; /* if PERF_SAMPLE_WEIGHT */
	/* __u64 data_src;    /\* if PERF_SAMPLE_DATA_SRC *\/ */
};

enum pbuftype { DRAMREAD = 0, NVMREAD = 1, WRITE = 2, NPBUFTYPES };

void *pebs_kswapd();
struct hemem_page *pebs_pagefault(void);
struct hemem_page *pebs_pagefault_unlocked(void);
void pebs_init(void);
void pebs_remove_page(struct hemem_page *page);
void pebs_stats();
void pebs_shutdown();

#endif /*  HEMEM_LRU_MODIFIED_H  */
