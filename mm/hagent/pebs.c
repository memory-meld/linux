#include "spsc.h"
#include <linux/percpu.h>
#include <linux/printk.h>

#include "module.h"
#include "pebs.h"
#include "sdh.h"

// DEFINE_PER_CPU(struct hagent_sample[2 << 20], collected_samples);
// DEFINE_PER_CPU(u64, next_sample_index);

void hagent_drain_pebs(struct hagent_info *info)
{
	static volatile unsigned long invalid_count = 0;
	for (int cpu = 0; cpu < num_online_cpus(); ++cpu) {
		struct hagent_sample sample = {};
		while (spsc_recv(info->sample_chan[cpu], &sample)) {
			if (!sample.va || sample.va == RHEAP_HASH_TOMESTONE) {
				++invalid_count;
				continue;
			}
			sdh_add(info->sdh, sample.va & HPAGE_MASK);
			if (sample.nth % 4096 == 0) {
				pr_info("drained %lu pebs samples %lu invalid",
					sample.nth, invalid_count);
				// sdh_show_topk(info->sdh);
			}
		}
	}
}
