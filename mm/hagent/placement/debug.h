#ifndef HAGENT_PLACEMENT_DEBUG_H
#define HAGENT_PLACEMENT_DEBUG_H

#include <linux/fs.h>

#include "histogram.h"
#include "pebs.h"
#include "utils.h"

static inline int __filp_close(struct file **file)
{
	if (file && !IS_ERR_OR_NULL(*file))
		TRY(filp_close(*file, NULL));
	return 0;
}

// DEFINE_CLASS(file, struct file *, __filp_close(&_T),
// 	     filp_open(filename, flags, mode), const char *filename, int flags,
// 	     umode_t mode);

static inline ssize_t debug_write_file(char const *name, void *buf, size_t len)
{
	if (!name || !buf) {
		return -EINVAL;
	}
	struct file *file __cleanup(__filp_close) =
		TRY(filp_open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644));
	return TRY(kernel_write(file, buf, len, 0));
}

enum folio_debug_info_timestamp {
	// Used for calculating the latency between migration and pebs hardware
	// first generated the sample
	FDTI_FIRST_FOUND,
	FDTI_COLLECTION,
	FDTI_MIGRATION,
	// Used for calculating how long does it take for the same sample to
	// occur again
	FDTI_LAST_FOUND,
	FDTI_MAX,
};

struct page_debug_info {
	struct {
		u64 total_accesses;
		u64 timestamps[FDTI_MAX];
	} *pages;
	u64 len;
	struct logarithmic_histogram reoccurrence_latency;
};
static inline void page_debug_info_drop(struct page_debug_info *info)
{
	kvfree(info->pages);
	memset(info, 0, sizeof(*info));
}
static int __must_check page_debug_info_init(struct page_debug_info *info)
{
	info->len = num_possible_pages();
	info->pages =
		TRY(kvcalloc(info->len, sizeof(*info->pages), GFP_KERNEL));
	return 0;
}
static int page_debug_info_mark_found(struct page_debug_info *info,
				      struct perf_sample *s, u64 now)
{
	if (!info->len || !info->pages) {
		return -EINVAL;
	}
	u64 pfn = PFN_DOWN(s->phys_addr);
	if (!pfn) {
		return -ENOENT;
	}
	typeof(info->pages) one = info->pages + pfn;
	one->total_accesses += 1;
	if (!one->timestamps[FDTI_FIRST_FOUND]) {
		one->timestamps[FDTI_FIRST_FOUND] = s->time;
	}
	if (one->timestamps[FDTI_LAST_FOUND]) {
		u64 reoccurrence = saturating_sub(
			s->time, one->timestamps[FDTI_LAST_FOUND]);
		logarithmic_histogram_push(&info->reoccurrence_latency,
					   reoccurrence);
	}
	one->timestamps[FDTI_LAST_FOUND] = s->time;
	if (!one->timestamps[FDTI_COLLECTION]) {
		one->timestamps[FDTI_COLLECTION] = now;
	}
	return 0;
}
static int page_debug_info_mark_migrated(struct page_debug_info *info, u64 pfn,
					 u64 now)
{
	if (!pfn) {
		return -ENOENT;
	}
	typeof(info->pages) one = info->pages + pfn;
	if (!one->timestamps[FDTI_MIGRATION]) {
		one->timestamps[FDTI_MIGRATION] = now;
	}
	return 0;
}
static void page_debug_info_print_debug(struct page_debug_info *info)
{
	struct logarithmic_histogram total_accesses = {},
				     collection_latency = {},
				     migration_latency = {};
	for (u64 i = 0; i < info->len; ++i) {
		typeof(info->pages) one = info->pages + i;
		logarithmic_histogram_push(&total_accesses,
					   one->total_accesses);
		if (!one->timestamps[FDTI_FIRST_FOUND] ||
		    !one->timestamps[FDTI_COLLECTION]) {
			continue;
		}
		logarithmic_histogram_push(
			&collection_latency,
			saturating_sub(one->timestamps[FDTI_COLLECTION],
				       one->timestamps[FDTI_FIRST_FOUND]));
		if (!one->timestamps[FDTI_MIGRATION]) {
			continue;
		}
		logarithmic_histogram_push(
			&migration_latency,
			saturating_sub(one->timestamps[FDTI_MIGRATION],
				       one->timestamps[FDTI_COLLECTION]));
	}
	pr_info("total_accesses histo:\n");
	logarithmic_histogram_print_debug(&total_accesses);
	pr_info("collection_latency histo:\n");
	logarithmic_histogram_print_debug(&collection_latency);
	pr_info("migration_latency histo:\n");
	logarithmic_histogram_print_debug(&migration_latency);
	pr_info("reoccurrence_latency histo:\n");
	logarithmic_histogram_print_debug(&info->reoccurrence_latency);
}

struct sample_debug_info {
	u64 size, cap;
	struct perf_sample data[0];
};
static inline void sample_debug_info_drop(struct sample_debug_info *info)
{
	kvfree(info);
}
static int __must_check sample_debug_info_init(struct sample_debug_info **info,
					       u64 cap)
{
	*info = kvzalloc(sizeof(**info) + sizeof(struct perf_sample) * cap,
			 GFP_KERNEL);
	if (!*info) {
		return -ENOMEM;
	}
	(*info)->cap = cap;
	return 0;
}
static int sample_debug_info_push(struct sample_debug_info *info,
				  struct perf_sample *s)
{
	if (info->size >= info->cap) {
		return -ENOSPC;
	}
	info->data[info->size++] = *s;
	return 0;
}
static ssize_t sample_debug_info_write_file(struct sample_debug_info *info,
					    char const *filename)
{
	return debug_write_file(filename, info->data,
				info->size * sizeof(struct perf_sample));
}

#endif // !HAGENT_PLACEMENT_DEBUG_H
