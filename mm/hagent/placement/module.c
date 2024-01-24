#include <linux/module.h>

#include "module.h"
#include "placement.h"
#include "utils.h"

ulong ring_buffer_pages = RING_BUFFER_PAGES;
module_param_named(ring_buffer_pages, ring_buffer_pages, ulong, 0644);
MODULE_PARM_DESC(
	ring_buffer_pages,
	"Number of pages allocated for the ring buffer, defaults to 1M");

ulong load_latency_sample_period = SAMPLE_PERIOD;
module_param_named(load_latency_sample_period, load_latency_sample_period,
		   ulong, 0644);
MODULE_PARM_DESC(load_latency_sample_period,
		 "Sample period for ldlat event, defaults to 17");

ulong load_latency_threshold = LOAD_LATENCY_THRESHOLD;
module_param_named(load_latency_threshold, load_latency_threshold, ulong, 0644);
MODULE_PARM_DESC(load_latency_threshold,
		 "Load latency threshold for ldlat event, defaults to 64");

ulong retired_stores_sample_period = SAMPLE_PERIOD;
module_param_named(retired_stores_sample_period, retired_stores_sample_period,
		   ulong, 0644);
MODULE_PARM_DESC(retired_stores_sample_period,
		 "Sample period for retired stores event, defaults to 17");

ulong streaming_decaying_sketch_width = SDS_WIDTH;
module_param_named(streaming_decaying_sketch_width,
		   streaming_decaying_sketch_width, ulong, 0644);
MODULE_PARM_DESC(streaming_decaying_sketch_width,
		 "Width for streaming decaying sketch, defaults to 8192");

ulong streaming_decaying_sketch_depth = SDS_DEPTH;
module_param_named(streaming_decaying_sketch_depth,
		   streaming_decaying_sketch_depth, ulong, 0644);
MODULE_PARM_DESC(streaming_decaying_sketch_depth,
		 "Depth for streaming decaying sketch, defaults to 4");

ulong migration_candidate_size = MIGRATION_NCANDIDATE;
module_param_named(indexable_heap_capacity, migration_candidate_size, ulong,
		   0644);
MODULE_PARM_DESC(indexable_heap_capacity,
		 "Capacity for indexable heap, defaults to 131072");

ulong migration_target_dram_access_percentile =
	MIGRATION_TARGET_DRAM_ACCESS_PERCENTILE;
module_param_named(migration_target_dram_access_percentile,
		   migration_target_dram_access_percentile, ulong, 0644);
MODULE_PARM_DESC(
	migration_target_dram_access_percentile,
	"Target percentile of DRAM accesses for migration, defaults to 95");

ulong migration_batch_size = MIGRATION_BATCH_SIZE;
module_param_named(migration_batch_size, migration_batch_size, ulong, 0644);
MODULE_PARM_DESC(migration_batch_size,
		 "Batch size for migration, defaults to 4096 pages");

bool asynchronous_architecture = ASYNCHRONOUS_ARCHITECTURE;
module_param_named(asynchronous_architecture, asynchronous_architecture, bool,
		   0644);
MODULE_PARM_DESC(asynchronous_architecture,
		 "Whether to use asynchronous architecture, defaults to true");

bool debug_log_samples = false;
module_param_named(debug_log_samples, debug_log_samples, bool, 0644);
MODULE_PARM_DESC(debug_log_samples,
		 "Log every collected pebs sample (only for debugging)");

bool debug_migration_latency = false;
module_param_named(debug_migration_latency, debug_migration_latency, bool,
		   0644);
MODULE_PARM_DESC(debug_migration_latency,
		 "Log migration latency (only for debugging)");

DEFINE_STATIC_KEY_TRUE(use_asynchronous_architecture);

static void intel_pmu_print_debug_all(void)
{
	int cpu;
	for_each_online_cpu(cpu) {
		void perf_event_print_debug(void);
		smp_call_on_cpu(cpu, (int (*)(void *))perf_event_print_debug,
				NULL, false);
	}
}

// static int __must_check node_phys_addr_range(int nid, void **begin, void **end)
// {
// 	pg_data_t *pgdat = NODE_DATA(nid);
// 	if (!pgdat) {
// 		return -EINVAL;
// 	}
// 	u64 start_pfn = pgdat->node_start_pfn;
// 	u64 end_pfn = pgdat->node_start_pfn + pgdat->node_spanned_pages;
// 	if (begin) {
// 		*begin = (void *)PFN_PHYS(start_pfn);
// 	}
// 	if (end) {
// 		*end = (void *)PFN_PHYS(end_pfn);
// 	}
// 	return 0;
// }
static struct placement __global_placement;

static int init(void)
{
	struct placement *p = &__global_placement;
	UNWRAP(placement_init(p));
	UNWRAP(placement_start(p));
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
