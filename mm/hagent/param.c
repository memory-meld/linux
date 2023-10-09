#include <linux/moduleparam.h>

unsigned long hagent_sdh_w = 8192;
module_param_named(sdh_w, hagent_sdh_w, ulong, 0644);
MODULE_PARM_DESC(sdh_w, "Width of the SDH");

unsigned long hagent_sdh_d = 2;
module_param_named(sdh_d, hagent_sdh_d, ulong, 0644);
MODULE_PARM_DESC(sdh_d, "Depth of the SDH");

unsigned long hagent_sdh_k = 2048;
module_param_named(sdh_k, hagent_sdh_k, ulong, 0644);
MODULE_PARM_DESC(sdh_k, "K of the SDH");

unsigned long hagent_event_config = 0x01cd;
module_param_named(event_config, hagent_event_config, ulong, 0644);
MODULE_PARM_DESC(
	event_config,
	"EventSel and UMask for the desired event, defaults to load latency");

unsigned long hagent_event_threshold = 64;
module_param_named(event_threshold, hagent_event_threshold, ulong, 0644);
MODULE_PARM_DESC(
	event_threshold,
	"Additional parameter for the selected config, defaults to load latency threshold for ldlat event");

unsigned long hagent_event_period = 0x10;
module_param_named(event_period, hagent_event_period, ulong, 0644);
MODULE_PARM_DESC(event_period, "PEBS sampling interval");

unsigned long hagent_channel_capacity = 1 << 16;
module_param_named(channel_capacity, hagent_channel_capacity, ulong, 0644);
MODULE_PARM_DESC(
	channel_capacity,
	"Capacity of the SPSC channel between sample collection and hotness identification");

bool hagent_dump_topk = 0x0;
module_param_named(dump_topk, hagent_dump_topk, bool, 0644);
MODULE_PARM_DESC(
	dump_topk,
	"Whether to dump the top-k hottest pages at profiling target exit");
