#include "linux/printk.h"
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/module.h>

#include "hagent.h"
#include "hook.h"
#include "hooked.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Junliang Hu");
MODULE_DESCRIPTION("Heterogeneous memory management guest agent");

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

static int __init hagent_module_init(void)
{
	BUG_ON(hagent_init());
	pr_info("hagent structure allocated");
	syscall_hook_install(__NR_exit_group, hagent_hooked_exit_group);
	pr_info("exit_group hook installed");
	syscall_hook_install(__NR_mmap, hagent_hooked_mmap);
	pr_info("mmap hook installed");
	return 0;
}

static void __exit hagent_module_exit(void)
{
	syscall_hook_remove(__NR_mmap);
	pr_info("mmap hook removed");
	syscall_hook_remove(__NR_exit_group);
	pr_info("exit_group hook removed");
	hagent_exit();
	pr_info("hagent structure deallocated");
}

module_init(hagent_module_init);
module_exit(hagent_module_exit);
