#ifndef HAGENT_PLACEMENT_IHEAP_H
#define HAGENT_PLACEMENT_IHEAP_H

#include <linux/kernel.h>
#include <linux/btree.h>
#include <linux/min_heap.h>
#include <linux/slab.h>

#include "module.h"

struct pair {
	u64 key, value;
};

static inline bool indexable_heap_pair_less(struct pair const *lhs,
					    struct pair const *rhs)
{
	if (lhs->key < rhs->key) {
		return true;
	} else if (lhs->key > rhs->key) {
		return false;
	} else {
		return lhs->value < rhs->value;
	}
}

static inline bool indexable_heap_pair_value_less(struct pair const *lhs,
						  struct pair const *rhs)
{
	return lhs->value < rhs->value;
}

static inline bool indexable_heap_pair_value_greater(struct pair const *lhs,
						     struct pair const *rhs)
{
	return lhs->value > rhs->value;
}

static inline void indexable_heap_pair_swap(struct pair *lhs, struct pair *rhs)
{
	swap(*lhs, *rhs);
}

struct indexable_heap {
	// The basic idea would be using a heap and an index table.
	// We can use linux's min_heap as the internal heap container.
	// Because it provides a customizable swap callback,
	// which allow us to update the key->pos index table.
	// The index table is a btree, because currently there is no convienient
	// in-memory array-based hash table implementation in kernel.
	// map key (u64) -> position (u64) in the heap
	struct btree_head64 index;
	// store (u64, u64) pairs
	struct min_heap heap;
	struct min_heap_callbacks heap_cbs;
};

static inline u64 indexable_heap_length(struct indexable_heap *h)
{
	return h->heap.nr;
}

static inline u64 indexable_heap_capacity(struct indexable_heap *h)
{
	return h->heap.size;
}

static inline int __must_check
indexable_heap_index_update_or_insert(struct indexable_heap *h, u64 key,
				      struct pair *p)
{
	int err = btree_update64(&h->index, key, p);
	if (err) {
		BUG_ON(err != -ENOENT);
		err = btree_insert64(&h->index, key, p, GFP_KERNEL);
	}
	return err;
}

static inline void indexable_heap_swap_and_update_index(struct min_heap *heap,
							struct pair *lhs,
							struct pair *rhs)
{
	struct indexable_heap *h =
		container_of(heap, struct indexable_heap, heap);

	swap(*lhs, *rhs);

	BUG_ON(indexable_heap_index_update_or_insert(h, lhs->key, lhs));
	BUG_ON(indexable_heap_index_update_or_insert(h, rhs->key, rhs));
}

static inline void indexable_heap_drop(struct indexable_heap *h)
{
	btree_destroy64(&h->index);
	kvfree(h->heap.data);
}

static inline int __must_check indexable_heap_init(struct indexable_heap *h,
						   bool min_heap, u64 cap)
{
	BUG_ON(cap == 0);
	*h = (struct indexable_heap) {
		.heap_cbs = {
			.elem_size = sizeof(struct pair),
			.less = min_heap ? (void *)indexable_heap_pair_value_less :
					   (void *)indexable_heap_pair_value_greater,
			.swp = (void *)indexable_heap_swap_and_update_index,
		},
		.heap =  {
			.data = kvcalloc(sizeof(struct pair), cap,
					 GFP_KERNEL),
			.nr = 0,
			.size = cap,
		}
	};
	btree_init64(&h->index);
	return 0;
}

static inline struct pair *indexable_heap_get(struct indexable_heap *h, u64 key)
{
	return btree_lookup64(&h->index, key);
}

static inline struct pair *indexable_heap_pop_back(struct indexable_heap *h)
{
	struct min_heap *heap = &h->heap;
	struct min_heap_callbacks *cbs = &h->heap_cbs;
	struct pair *back = min_heap_back(heap, cbs);
	if (back) {
		btree_remove64(&h->index, back->key);
		min_heap_pop_back(heap, cbs);
		return back;
	}
	return NULL;
}

static inline void indexable_heap_print_debug(struct indexable_heap *h)
{
	struct min_heap *heap = &h->heap;
	struct min_heap_callbacks *cbs = &h->heap_cbs;
	pr_info("%s: cap=%d len=%d data=0x%px slots=[", __func__, heap->size,
		heap->nr, heap->data);
	u64 skip = 0;
	for (struct pair *i = min_heap_begin(heap);
	     i != min_heap_end(heap, cbs); ++i) {
		if (i->value < 5) {
			++skip;
			continue;
		}
		pr_cont(" (0x%llx, %llu),", i->key, i->value);
	}
	pr_cont("]\n");
	pr_info("%s: skipped %llu elements whose count < 5\n", __func__, skip);
}

// Track a new key or update the key's value.
// Return the old pair if the key already exists or the old pair is replaced.
// Otherwise, return (-ENOENT, -ENOENT) if a new pair is inserted,
// or (-EFBIG, -EFBIG) if the replace attempt failed.
// For a min_heap, it's used to track the top-k largest values.
// So, when we find a value larger than heap top, we should replace it.
static inline struct pair indexable_heap_insert(struct indexable_heap *h,
						struct pair const *elem)
{
	// pr_info_ratelimited("%s: key=0x%llx, value=%llu\n", __func__, elem->key,
	// 		    elem->value);
	struct pair *old = indexable_heap_get(h, elem->key), ret;
	struct min_heap *heap = &h->heap;
	struct min_heap_callbacks *cbs = &h->heap_cbs;
	if (old) {
		ret = *old;
		// the value for the existing key does not change
		if (old->value == elem->value) {
			return ret;
		} else {
			// update an existing key
			old->value = elem->value;
			int pos = ((void *)old - heap->data) / cbs->elem_size;
			if (cbs->less(elem, old)) {
				// decrease the value in a min_heap, we should sift up
				min_heap_sift_up(heap, pos, cbs);
			} else {
				// icrease the value in a min_heap, we should sift down
				min_heapify(heap, pos, cbs);
			}
			return ret;
		}
	} else {
		// inserting an new key
		u64 len = indexable_heap_length(h),
		    cap = indexable_heap_capacity(h);
		if (len < cap) {
			// we have space for insertion, we can directly insert it
			min_heap_push(heap, elem, cbs);
			return (struct pair){ -ENOENT, -ENOENT };
		} else if (len == cap) {
			// If the new value is larger heap top, we should replace it,
			// so that the heap always keep the top-k largest values.
			struct pair *begin = min_heap_begin(heap);
			ret = *begin;
			if (cbs->less(begin, elem)) {
				// old top is indeed smaller
				min_heap_pop_push(heap, elem, cbs);
				btree_remove64(&h->index, ret.key);
				return ret;
			} else {
				// the replacement of top failed
				return (struct pair){ -EFBIG, -EFBIG };
			}
		} else {
			// we can never overflow the capacity
			BUG();
		}
	}
}

static inline void indexable_heap_update_param(void)
{
	if (migration_candidate_size == MIGRATION_NCANDIDATE) {
		// Setting candiate queue size to 10% of DRAM size
		// It will be the upper bound on the batch size of migration
		u64 dram_spanned = DRAM_NODE->node_spanned_pages;
		migration_candidate_size = dram_spanned / 10;
		pr_info("%s: migration_candidate_size=%lu\n", __func__,
			migration_candidate_size);
	}
}

#endif // !HAGENT_PLACEMENT_IHEAP_H
