#include <linux/printk.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/xxhash.h>

#include "rheap.h"

// rheap entry
struct rheap_pair {
	unsigned long k, v;
};
// heap data storage entry
typedef struct {
	unsigned long k, v;
} data_pair_t;
// hash index entry
typedef struct {
	unsigned long k, v;
} hash_pair_t;
typedef unsigned long rhindex_t;

// a reverse queriable min heap
// the interface we need:
// insert, find, delete, peak, pop
struct rheap {
	unsigned long len, dcap, hcap;
	// the heap storage, storing (key, value)
	data_pair_t *d;
	// an array for the reverse index hashtable, storing (key, index)
	hash_pair_t h[];
};

static rhindex_t rheap_parent(rhindex_t i)
{
	return (i - 1) / 2;
}

static rhindex_t rheap_lchild(rhindex_t i)
{
	return (i + 1) * 2 - 1;
}

static rhindex_t rheap_rchild(rhindex_t i)
{
	return (i + 1) * 2;
}

struct rheap *rheap_new(unsigned long cap)
{
	cap = roundup_pow_of_two(cap);
	// the hashtable is allocated 16x the capacity to avoid collision
	unsigned long size = sizeof(struct rheap) +
			     16 * cap * sizeof(hash_pair_t) +
			     cap * sizeof(data_pair_t);
	struct rheap *rh = kvzalloc(size, GFP_KERNEL);
	BUG_ON(!rh);
	rh->dcap = cap;
	rh->hcap = 8 * cap;
	rh->d = (void *)rh + rh->hcap * sizeof(hash_pair_t);
	return rh;
}

void rheap_drop(struct rheap *rh)
{
	kvfree(rh);
}

unsigned long rheap_len(struct rheap *rh)
{
	return rh->len;
}

static data_pair_t *rheap_at(struct rheap *rh, rhindex_t i)
{
	return &rh->d[i];
}

static hash_pair_t *rheap_hash_at(struct rheap *rh, rhindex_t i)
{
	return &rh->h[i];
}

void rheap_show_all(struct rheap *h)
{
	for (unsigned long i = 0; i < h->len; ++i) {
		data_pair_t *d = rheap_at(h, i);
		pr_cont(" (0x%lx, %lu)", d->k, d->v);
	}
}

static unsigned rheap_hash(rhkey_t key)
{
	// The key are 2MiB page address in our case, lower 21bits are useless
	// return hash_64_generic(key ^ SEEDS[32], 64 - 21);
	return xxh32(&key, sizeof(key), 0x696b378b);
}

// Find the given key in the rheap.
// The value in heap can then be checked via access the location returned
static hash_pair_t *rheap_hash_find(struct rheap *rh, rhkey_t const key)
{
	hash_pair_t *he;
	unsigned long cap = rh->hcap, i = rheap_hash(key) % cap, j = i;
	// linear probing
	while (he = rheap_hash_at(rh, i), !(he->k == key || he->k == 0)) {
		if (++i == cap)
			i = 0;
		if (i == j) {
			pr_err("%s(rh=0x%px,key=0x%lx) table full: already checked every slot",
			       __func__, rh, key);
			BUG();
		}
	}
	// We have found:
	// - the inserted slot for the given key, or
	// - an empty slot where the key should be inserted
	return he;
}

static bool rheap_hash_entry_empty(hash_pair_t *s)
{
	return (s->k == 0 || s->k == RHEAP_HASH_TOMESTONE) && s->v == 0;
}

hash_pair_t *rheap_hash_end(struct rheap *rh)
{
	return rheap_hash_at(rh, rh->hcap);
}

hash_pair_t *rheap_hash_begin(struct rheap *rh)
{
	return rheap_hash_at(rh, 0);
}

// Delete the given node from the hash table
static void rheap_hash_delete(struct rheap *rh, hash_pair_t *he)
{
	hash_pair_t *next = he + 1;
	if (next == rheap_hash_end(rh)) {
		next = rheap_hash_begin(rh);
	}
	if (next->k) {
		he->k = RHEAP_HASH_TOMESTONE, he->v = 0;
		// Put a tombstone here so that subsequent keys could still be accessed.
		// Tf very unfortunate, the tombstone still should never be hashed to.
		// This is the only key that our hashtable fails to serve (false positive).
		// Spending O(1) time to ensure this never happens is fine.
		BUG_ON(rheap_hash_find(rh, RHEAP_HASH_TOMESTONE) == he);
	} else {
		he->k = 0, he->v = 0;
	}
}

// static void rheap_hash_insert(struct rheap *rheap, )

rhvalue_t rheap_get(struct rheap *rh, rhkey_t const key)
{
	hash_pair_t const *he = rheap_hash_find(rh, key);
	if (!he->k || he->k == RHEAP_HASH_TOMESTONE) {
		return 0;
	}
	return rheap_at(rh, he->v)->v;
}

void rheap_peek(struct rheap const *rh, rhkey_t *key, rhvalue_t *value)
{
	data_pair_t const *e = rh->d;
	if (key) {
		*key = e->k;
	}
	if (value) {
		*value = e->v;
	}
}

bool rheap_full(struct rheap const *rh)
{
	return rh->len == rh->dcap;
}

// swap parent with child if child's value is less than parent
// return true if a swap happened
static bool rheap_swap_if(struct rheap *rh, rhindex_t parent, rhindex_t child)
{
	data_pair_t *pde = rheap_at(rh, parent), *cde = rheap_at(rh, child);
	if (pde->v <= cde->v) {
		return false;
	}
	// parent and child must be in the hashtable
	hash_pair_t *phe = rheap_hash_find(rh, pde->k);
	if (phe->k != pde->k || phe->v != parent) {
		hash_pair_t *che = rheap_hash_find(rh, cde->k);
		pr_info("%s(rh=%px,parent=%lu,child=%lu) "
			"parent data (key=0x%lx,value=%lu) hash (key=0x%lx,value=%lu) "
			"child data (key=0x%lx,value=%lu) hash (key=0x%lx,value=%lu)",
			__func__, rh, parent, child, pde->k, pde->v, phe->k,
			phe->v, cde->k, cde->v, che->k, che->v);
		BUG();
	}
	phe->v = child;
	hash_pair_t *che = rheap_hash_find(rh, cde->k);
	if (che->k != cde->k || che->v != child) {
		pr_info("%s(rh=%px,parent=%lu,child=%lu) "
			"parent data (key=0x%lx,value=%lu) hash (key=0x%lx,value=%lu) "
			"child data (key=0x%lx,value=%lu) hash (key=0x%lx,value=%lu)",
			__func__, rh, parent, child, pde->k, pde->v, phe->k,
			phe->v, cde->k, cde->v, che->k, che->v);
		BUG();
	}
	che->v = parent;
	swap(*pde, *cde);
	return true;
}

static bool rheap_swap(struct rheap *rh, rhindex_t parent, rhindex_t child)
{
	data_pair_t *pde = rheap_at(rh, parent), *cde = rheap_at(rh, child);
	// parent and child must be in the hashtable
	hash_pair_t *phe = rheap_hash_find(rh, pde->k);
	BUG_ON(phe->k != pde->k || phe->v != parent);
	phe->v = child;
	hash_pair_t *che = rheap_hash_find(rh, cde->k);
	BUG_ON(che->k != cde->k || che->v != child);
	che->v = parent;
	swap(*pde, *cde);
	return true;
}

static void rheap_sift_up(struct rheap *rh, rhindex_t i)
{
	while (i) {
		// i > 0 must hold
		rhindex_t p = rheap_parent(i);
		// no need to check p
		rheap_swap_if(rh, p, i);
		i = p;
	}
}

static void rheap_sift_down(struct rheap *rh, rhindex_t i)
{
	while (i < rh->len) {
		rhindex_t l = rheap_lchild(i), r = rheap_rchild(i);
		// if left child index is already out of range
		// then the right child will also be out of range
		if (l >= rh->len) {
			break;
		}
		data_pair_t *le = rheap_at(rh, l), *re = rheap_at(rh, r);
		// if right child is out of range,
		// it's possible left child is not
		// we need to compare with left
		if (r >= rh->len || le->v < re->v) {
			rheap_swap_if(rh, i, l);
			i = l;
		} else {
			rheap_swap_if(rh, i, r);
			i = r;
		}
	}
}

void rheap_push(struct rheap *rh, rhkey_t const key, rhvalue_t const value)
{
	// pr_info("%s(rh=%px,key=0x%lx,value=%lu)", __func__, rh, key, value);
	rhindex_t i = rh->len++;
	data_pair_t de = { .k = key, .v = value };
	*rheap_at(rh, i) = de;
	hash_pair_t *he = rheap_hash_find(rh, key), new = { .k = key, .v = i };
	if (!rheap_hash_entry_empty(he)) {
		pr_info("%s(rh=%px,key=0x%lx,value=%lu) found hashtable entry (key=0x%lx,value=%lu)",
			__func__, rh, key, value, he->k, he->v);
		BUG();
	}
	// if (!rheap_hash_entry_empty(he)) {
	// 	pr_err("%s: pushing an already existed index entry (key=0x%lx,index=%lu) backing (key=0x%lx,value=%lu) new index (key=0x%lx,value=%lu)",
	// 	       __func__, he->k, he->v, rheap_at(rh, he->v)->k,
	// 	       rheap_at(rh, he->v)->v, key, i);
	// }
	*he = new;
	rheap_sift_up(rh, i);
	BUG_ON(rheap_get(rh, key) != value);
}

// delete the entry identified by the given key
// bool rheap_delete(struct rheap *rh, rhkey_t key)
// {
// 	switch (rh->len) {
// 	case 0:
// 		return false;
// 	case 1: {
// 		rhde_t *e = rheap_at(rh, 0), zero = {};
// 		if (e->k != key) {
// 			return false;
// 		}
// 		rheap_hash_delete(rh, rheap_hash_find(rh, e->k));
// 		*e = zero;
// 		rh->len -= 1;
// 		return true;
// 	}
// 	default: {
// 		rhhe_t *h = rheap_hash_find(rh, key);
// 		if (h->k != key) {
// 			return false;
// 		}
// 		rhindex_t i = h->v, last = rh->len - 1;
// 		rheap_swap(rh, i, last);
// 		rheap_hash_delete(rh, h);
// 		rhde_t *le = rheap_at(rh, last), zero = {};
// 		*le = zero;
// 		rh->len -= 1;
// 		rheap_sift_down(rh, i);
// 		return true;
// 	}
// 	}
// }

bool rheap_update(struct rheap *rh, rhkey_t const key, rhvalue_t const value)
{
	switch (rh->len) {
	case 0:
		BUG();
		return false;
	case 1: {
		data_pair_t *e = rheap_at(rh, 0),
			    new = { .k = key, .v = value };
		if (e->k != key) {
			return false;
		}
		*e = new;
		return true;
	}
	default: {
		hash_pair_t *h = rheap_hash_find(rh, key);
		if (h->k != key) {
			BUG();
			return false;
		}
		rhindex_t i = h->v, last = rh->len - 1;
		rheap_swap(rh, i, last);
		data_pair_t *le = rheap_at(rh, last),
			    new = { .k = key, .v = value };
		*le = new;
		rheap_sift_down(rh, i);
		rheap_sift_up(rh, last);
		return true;
	}
	}
}

bool rheap_replace(struct rheap *rh, rhkey_t const old_key, rhkey_t const key,
		   rhvalue_t const value)
{
	data_pair_t new = { .k = key, .v = value };
	switch (rh->len) {
	case 0:
		BUG();
		return false;
	case 1: {
		data_pair_t *e = rheap_at(rh, 0);
		if (e->k != old_key) {
			return false;
		}
		*e = new;
		rheap_hash_delete(rh, rheap_hash_find(rh, old_key));
		hash_pair_t r = { .k = new.k, .v = 0 };
		*rheap_hash_find(rh, new.k) = r;
		return true;
	}
	default: {
		hash_pair_t *h = rheap_hash_find(rh, old_key);
		if (h->k != old_key) {
			// trying to replace a non-existent key
			pr_info("%s(rh=%px,old_key=0x%lx,key=0x%lx,value=%lu) replace a non-existent slot (key=0x%lx,value=%lu)",
				__func__, rh, old_key, key, value, h->k, h->v);
			BUG();
			return false;
		}
		rhindex_t i = h->v, last = rh->len - 1;
		rheap_swap(rh, i, last);
		data_pair_t *le = rheap_at(rh, last);
		*le = new;
		rheap_hash_delete(rh, h);
		hash_pair_t r = { .k = key, .v = last };
		*rheap_hash_find(rh, key) = r;
		rheap_sift_down(rh, i);
		rheap_sift_up(rh, last);
		// insert the reverse index for new
		return true;
	}
	}
}

bool rheap_delete(struct rheap *rh, rhkey_t const key)
{
	data_pair_t zero = {};
	switch (rh->len) {
	case 0:
		BUG();
		return false;
	case 1: {
		data_pair_t *e = rheap_at(rh, 0);
		if (e->k != key) {
			return false;
		}
		*e = zero;
		rheap_hash_delete(rh, rheap_hash_find(rh, key));
		rh->len = 0;
		return true;
	}
	default: {
		hash_pair_t *h = rheap_hash_find(rh, key);
		if (h->k != key) {
			// trying to delete a non-existent key
			pr_info("%s(rh=%px,key=0x%lx) delete a non-existent key (key=0x%lx,value=%lu)",
				__func__, rh, key, h->k, h->v);
			BUG();
			return false;
		}
		rhindex_t i = h->v, last = rh->len - 1;
		rheap_swap(rh, i, last);
		data_pair_t *le = rheap_at(rh, last);
		*le = zero;
		rheap_hash_delete(rh, h);
		rh->len -= 1;
		rheap_sift_down(rh, i);
		return true;
	}
	}
}
