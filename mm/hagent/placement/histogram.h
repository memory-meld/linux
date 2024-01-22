#ifndef HAGENT_PLACEMENT_HISTOGRAM_H
#define HAGENT_PLACEMENT_HISTOGRAM_H

#include <linux/kernel.h>
#include <linux/string.h>

struct logarithmic_histogram {
	u64 buckets[64 + 1];
	u64 len;
};

static inline int logarithmic_histogram_init(struct logarithmic_histogram *h)
{
	memset(h, 0, sizeof(*h));
	return 0;
}

static inline void logarithmic_histogram_drop(struct logarithmic_histogram *h)
{
	memset(h, 0, sizeof(*h));
}

static inline void logarithmic_histogram_push(struct logarithmic_histogram *h,
					      u64 val)
{
	++h->buckets[val ? 1 + ilog2(val) : 0];
	++h->len;
}

static inline void
logarithmic_histogram_print_debug(struct logarithmic_histogram *h)
{
	for (u64 i = 0; i < ARRAY_SIZE(h->buckets); ++i) {
		u64 val = h->buckets[i];
		if (!val) {
			continue;
		}
		pr_info("%s: [%llu, %llu) = %llu\n", __func__,
			i ? 1ull << (i - 1) : 0, i ? 1ull << i : 1, val);
	}
}

#endif // !HAGENT_PLACEMENT_HISTOGRAM_H
