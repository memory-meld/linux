#ifndef HAGENT_PLACEMENT_UTILS_H
#define HAGENT_PLACEMENT_UTILS_H

#include <linux/cleanup.h>
#include <linux/bug.h>
#include <linux/err.h>
#include <linux/errname.h>
#include <linux/swap.h>
#include <linux/sched/clock.h>

#define TRY(exp)                                                           \
	({                                                                 \
		__typeof__((exp)) __err = (exp);                           \
		if ((u64)(__err) >= (u64)(-MAX_ERRNO)) {                   \
			pr_err_ratelimited(                                \
				"[%s:%d]%s failed with error %lld: %s\n",  \
				__FILE__, __LINE__, __func__, -(s64)__err, \
				errname(-(s64)__err));                     \
			return (s64)(__err);                               \
		}                                                          \
		__err;                                                     \
	})

#define UNWRAP(exp)                                                        \
	({                                                                 \
		__typeof__((exp)) __err = (exp);                           \
		if ((u64)(__err) >= (u64)(-MAX_ERRNO)) {                   \
			pr_err_ratelimited(                                \
				"[%s:%d]%s failed with error %lld: %s\n",  \
				__FILE__, __LINE__, __func__, -(s64)__err, \
				errname(-(s64)__err));                     \
			dump_stack();                                      \
			BUG();                                             \
		}                                                          \
		__err;                                                     \
	})

// return a - b when a >= b, return 0 when b > a
#define saturating_sub(a, b)            \
	({                              \
		typeof(a) __aa = (a);   \
		typeof(b) __bb = (b);   \
		__aa - min(__aa, __bb); \
	})

#define checked_div(a, b)                                   \
	({                                                  \
		__typecheck(a, b);                          \
		typeof(a) __aa = (a);                       \
		typeof(b) __bb = (b);                       \
		__bb ? __aa / __bb : ({                     \
			typeof(a) __mm = (typeof(a))~0U;    \
			__mm > 0 ?: (typeof(a))(__mm >> 1); \
		});                                         \
	})

static inline u64 num_possible_pages(void)
{
	u64 spanned = 0;
	int nid;
	for_each_node_state(nid, N_MEMORY) {
		spanned += NODE_DATA(nid)->node_spanned_pages;
	}
	return spanned;
}

DEFINE_LOCK_GUARD_0(lru, lru_cache_disable(), lru_cache_enable());

// #define DEFINE_CLASS(_name, _type, _exit, _init, _init_args...)     \
// 	typedef _type class_##_name##_t;                            \
// 	static inline void class_##_name##_destructor(_type *p)     \
// 	{                                                           \
// 		_type _T = *p;                                      \
// 		_exit;                                              \
// 	}                                                           \
// 	static inline _type class_##_name##_constructor(_init_args) \
// 	{                                                           \
// 		_type t = _init;                                    \
// 		return t;                                           \
// 	}
struct vmevent {
	enum vm_event_item item;
	u64 begin;
};
static inline struct vmevent vmevent_ctor(enum vm_event_item item)
{
	struct vmevent t = { .begin = local_clock(), .item = item };
	return t;
}
DEFINE_CLASS(vmevent, struct vmevent,
	     count_vm_events(_T.item, local_clock() - _T.begin),
	     vmevent_ctor(item), enum vm_event_item item);

#endif // !HAGENT_PLACEMENT_UTILS_H
