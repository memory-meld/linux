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

DEFINE_LOCK_GUARD_0(lru, lru_cache_disable(), lru_cache_enable());

enum mt19937_config {
	MT19937_SEED = 0x990124ULL,
	MT19937_NN = 312,
	MT19937_MM = 156,
	MT19937_MATRIX_A = 0xB5026F5AA96619E9ULL,
	// Most significant 33 bits
	MT19937_UM = 0xFFFFFFFF80000000ULL,
	// Least significant 31 bits
	MT19937_LM = 0x7FFFFFFFULL,
};
static DEFINE_PER_CPU(u64[MT19937_NN], mt19947_mt) = {};
static DEFINE_PER_CPU(u64, mt19947_mti) = MT19937_NN + 1;

// clang-format off
static inline u64 mt19937(void)
{
	static u64 const mag01[2] = { 0ULL, MT19937_MATRIX_A };
	// The array for the state vector
	u64 *mt = this_cpu_ptr(mt19947_mt);
	u64 mti = this_cpu_read(mt19947_mti);
	// mti==NN+1 means mt[NN] is not initialized
	if (mti >= MT19937_NN) {
		// generate NN words at one time
		// if init_genrand64() has not been called,
		// a default initial seed 5489ULL is used
		if (mti == MT19937_NN + 1) {
			mt[0] = MT19937_SEED;
			for (mti = 1; mti < MT19937_NN; ++mti) {
				mt[mti] = 6364136223846793005ULL * (mt[mti - 1] ^ (mt[mti - 1] >> 62)) + mti;
			}
		}
		for (int i = 0; i < MT19937_NN - MT19937_MM; i++) {
			u64 x = (mt[i] & MT19937_UM) | (mt[i + 1] & MT19937_LM);
			mt[i] = mt[i + MT19937_MM] ^ (x >> 1) ^ mag01[x & 1ULL];
		}
		for (int i = MT19937_NN - MT19937_MM; i < MT19937_NN - 1; i++) {
			u64 x = (mt[i] & MT19937_UM) | (mt[i + 1] & MT19937_LM);
			mt[i] = mt[i + (MT19937_MM - MT19937_NN)] ^ (x >> 1) ^ mag01[x & 1ULL];
		}
		u64 x = (mt[MT19937_NN - 1] & MT19937_UM) | (mt[0] & MT19937_LM);
		mt[MT19937_NN - 1] = mt[MT19937_MM - 1] ^ (x >> 1) ^ mag01[x & 1ULL];
		mti = 0;
	}
	u64 x = mt[mti++];
	this_cpu_write(mt19947_mti, mti);
	x ^= (x >> 29) & 0x5555555555555555ULL; x ^= (x << 17) & 0x71D67FFFEDA60000ULL;
	x ^= (x << 37) & 0xFFF7EEE000000000ULL; x ^= (x >> 43);
	return x;
}

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
