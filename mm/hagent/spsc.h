#ifndef __MM_HAGENT_SPSC_H
#define __MM_HAGENT_SPSC_H

#include <linux/slab.h>
#include "pebs.h"

typedef struct hagent_sample spsc_elem_t;

struct spsc {
	ulong cap;
	// tx: next index for send
	// rx: next index to receive
	// pad the struct to the align of spsc_elem_t
	volatile ulong tx, rx, _;
	spsc_elem_t elems[];
};

inline static struct spsc *spsc_new(ulong nelems)
{
	ulong size = sizeof(struct spsc) + nelems * sizeof(spsc_elem_t);
	struct spsc *new = kvzalloc(size, GFP_KERNEL), init = {
		.cap = nelems,
	};
	BUG_ON(!new);
	*new = init;
	return new;
}

inline static void spsc_send(struct spsc *chan, spsc_elem_t *elem)
{
	// first put the sample in the output buffer
	chan->elems[chan->tx % chan->cap] = *elem;
	// make that sample public
	// if tear heappens, we only lost one sample, it's not a big deal
	chan->tx += 1;
}

// Return true means the recv is a success, otherwise, the channel is empty.
inline static bool spsc_recv(struct spsc *chan, spsc_elem_t *out)
{
	static struct hagent_sample zero = {};
	if (!(chan->rx < chan->tx)) {
		return false;
	}
	spsc_elem_t *slot = &chan->elems[chan->rx % chan->cap];
	*out = *slot;
	*slot = zero;
	chan->rx += 1;
	return true;
}

inline static void spsc_drop(struct spsc *chan)
{
	if (chan) {
		kvfree(chan);
	}
}

#endif
