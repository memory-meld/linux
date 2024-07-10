#include <linux/perf_event.h>
#include <linux/ring_buffer.h>
#include <linux/syscalls.h>
#include "internal.h"

extern void ring_buffer_attach(struct perf_event *event,
			       struct perf_buffer *rb);
extern void perf_event_init_userpage(struct perf_event *event);

int htmm__perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
			  int group_fd, unsigned long flags)
{
	return __perf_event_open(attr, pid, cpu, group_fd, flags);
}

/* allocates perf_buffer instead of calling perf_mmap() */
int htmm__perf_event_init(struct perf_event *event, unsigned long nr_pages)
{
	struct perf_buffer *rb = NULL;
	int ret = 0, flags = 0;

	if (event->cpu == -1 && event->attr.inherit)
		return -EINVAL;

	ret = security_perf_event_read(event);
	if (ret)
		return ret;

	if (nr_pages != 0 && !is_power_of_2(nr_pages))
		return -EINVAL;

	WARN_ON_ONCE(event->ctx->parent_ctx);
	mutex_lock(&event->mmap_mutex);

	WARN_ON(event->rb);

	rb = rb_alloc(nr_pages,
		      event->attr.watermark ? event->attr.wakeup_watermark : 0,
		      event->cpu, flags);
	if (!rb) {
		ret = -ENOMEM;
		goto unlock;
	}

	ring_buffer_attach(event, rb);
	perf_event_init_userpage(event);
	perf_event_update_userpage(event);

unlock:
	if (!ret) {
		atomic_inc(&event->mmap_count);
	}
	mutex_unlock(&event->mmap_mutex);
	return ret;
}

#ifdef CONFIG_HTMM
#include <linux/htmm.h>
SYSCALL_DEFINE2(htmm_start, pid_t, pid, int, node)
{
	return ksamplingd_init(pid, node);
}
SYSCALL_DEFINE1(htmm_end, pid_t, pid)
{
	ksamplingd_exit();
	return 0;
}
#else
SYSCALL_DEFINE2(htmm_start, pid_t, pid, int, node)
{
	return 0;
}
SYSCALL_DEFINE1(htmm_end, pid_t, pid)
{
	return 0;
}
#endif
