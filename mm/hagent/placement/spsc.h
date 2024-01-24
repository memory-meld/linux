#ifndef HAGENT_PLACEMENT_SPSC_H
#define HAGENT_PLACEMENT_SPSC_H

#include <linux/kernel.h>
#include <linux/slab.h>

struct spsc {
	u64 head, tail, size;
	void *buffer;
};

static inline void spsc_drop(struct spsc *ch)
{
	pr_info("%s: ch=0x%px buffer=0x%px\n", __func__, ch, ch->buffer);
	kvfree(ch->buffer);
}

static inline int __must_check spsc_init(struct spsc *ch, u64 size)
{
	ch->head = ch->tail = 0;
	ch->size = size;
	ch->buffer = kvmalloc(size, GFP_KERNEL);
	pr_info("%s: ch=0x%px size=%llu buffer=0x%px\n", __func__, ch, size,
		ch->buffer);
	if (!ch->buffer) {
		return -ENOMEM;
	}
	return 0;
}

// We will assume the buffer size is the multiple of the element size
// Producer side only modify the head pointer.
static inline int __must_check spsc_push(struct spsc *ch, void const *buf,
					 u64 len)
{
	u64 head = READ_ONCE(ch->head), diff = head - READ_ONCE(ch->tail);
	if (diff && diff % ch->size == 0) {
		return -ENOMEM;
	}
	memcpy(ch->buffer + head % ch->size, buf, len);
	WRITE_ONCE(ch->head, head + len);
	return 0;
}

// Consumer side only modify the tail pointer.
static inline int __must_check spsc_pop(struct spsc *ch, void *buf, u64 len)
{
	u64 tail = READ_ONCE(ch->tail);
	if (READ_ONCE(ch->head) == tail) {
		return -EAGAIN;
	}
	memcpy(buf, ch->buffer + tail % ch->size, len);
	WRITE_ONCE(ch->tail, tail + len);
	return 0;
}

#endif // !HAGENT_PLACEMENT_SPSC_H
