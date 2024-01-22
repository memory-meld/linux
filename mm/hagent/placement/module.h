#ifndef HAGENT_PLACEMENT_MODULE_H
#define HAGENT_PLACEMENT_MODULE_H

#include "pebs.h"

#define DRAM_NID (first_node(node_states[N_MEMORY]))
#define PMEM_NID (last_node(node_states[N_MEMORY]))
#define DRAM_NODE (NODE_DATA(DRAM_NID))
#define PMEM_NODE (NODE_DATA(PMEM_NID))

enum module_param_defaults {
	RING_BUFFER_PAGES = 8ul << 20 >> PAGE_SHIFT,
	SAMPLE_PERIOD = 17,
	LOAD_LATENCY_THRESHOLD = 64,
	SPSC_NELEMS = (2ul << 20) / sizeof(struct perf_sample),
	SDS_WIDTH = 8192,
	SDS_DEPTH = 4,
	MIGRATION_NCANDIDATE = 131072,
	MIGRATION_TARGET_DRAM_ACCESS_PERCENTILE = 95,
	MIGRATION_BATCH_SIZE = 4096,
};
extern ulong ring_buffer_pages;
extern ulong load_latency_sample_period;
extern ulong load_latency_threshold;
extern ulong retired_stores_sample_period;
extern ulong streaming_decaying_sketch_width;
extern ulong streaming_decaying_sketch_depth;
extern ulong migration_candidate_size;
extern ulong migration_target_dram_access_percentile;
extern ulong migration_batch_size;
extern bool debug_log_samples;
extern bool debug_migration_latency;

#endif // !HAGENT_PLACEMENT_MODULE_H
