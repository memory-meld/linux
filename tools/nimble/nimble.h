#ifndef NIMBLE_H
#define NIMBLE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

enum {
	NR_MM_MANAGE = 511,

	MPOL_NF_MOVE = (1 << 1),
	MPOL_NF_MOVE_ALL = (1 << 2),
	// Use multi-threaded page copy routine
	MPOL_NF_MOVE_MT = (1 << 6),
	// Move pages in a batch
	MPOL_NF_MOVE_CONCUR = (1 << 7),
	// Exchange pages
	MPOL_NF_EXCHANGE = (1 << 8),
	// Shrink lists
	MPOL_NF_SHRINK_LISTS = (1 << 9),
	// All possible bits
	MPOL_NF_VALID =
		(MPOL_NF_MOVE | MPOL_NF_MOVE_ALL | MPOL_NF_MOVE_MT |
		 MPOL_NF_MOVE_CONCUR | MPOL_NF_EXCHANGE | MPOL_NF_SHRINK_LISTS),

	MAX_NUMNODES = 1024,
	BITSPERLONG = (8 * sizeof(unsigned long)),
	BITMAP_LEN = MAX_NUMNODES / BITSPERLONG,

	DRAM_NODE = 0,
	PMEM_NODE = 1,
};

#define UNWRAP(exp)                                                             \
	({                                                                      \
		typeof(exp) ret = (exp);                                        \
		void *erased = (void *)(long)ret;                               \
		char const *fmt =                                               \
			__builtin_classify_type(ret) == 5 ?                     \
				"[%s:%d](%s) `%s` Returned `%p` Error %d %s" :  \
				"[%s:%d](%s) `%s` Returned `%lld` Error %d %s"; \
		if (errno && (erased == NULL || erased == (void *)-1)) {        \
			fprintf(stderr, fmt, __FILE__, __LINE__, __func__,      \
				#exp, erased, errno, strerror(errno));          \
			abort();                                                \
		}                                                               \
		ret;                                                            \
	})

inline static unsigned int _getbit(unsigned long const *maskp,
				   unsigned long size, unsigned int n)
{
	if (n < size)
		return (maskp[n / BITSPERLONG] >> (n % BITSPERLONG)) & 1;
	else
		return 0;
}

inline static void _setbit(unsigned long *maskp, unsigned long size,
			   unsigned int n, unsigned int v)
{
	if (n < size) {
		if (v)
			maskp[n / BITSPERLONG] |= 1UL << (n % BITSPERLONG);
		else
			maskp[n / BITSPERLONG] &= ~(1UL << (n % BITSPERLONG));
	}
}

inline static long mm_manage(pid_t pid, unsigned long nr_pages, int from,
			     int to, int flags)
{
	static unsigned long bfrom[BITMAP_LEN] = {};
	static unsigned long bto[BITMAP_LEN] = {};
	for (int i = 0; i < MAX_NUMNODES; ++i) {
		if (i == from) {
			_setbit(bfrom, MAX_NUMNODES, i, 1);
		} else if (i == to) {
			_setbit(bto, MAX_NUMNODES, i, 1);
		} else {
			_setbit(bfrom, MAX_NUMNODES, i, 0);
			_setbit(bto, MAX_NUMNODES, i, 0);
		}
	}
	return UNWRAP(syscall(NR_MM_MANAGE, pid, nr_pages, MAX_NUMNODES, bfrom,
			      bto, flags));
}

inline static void sleep_ms(unsigned int ms)
{
	if (!ms)
		return;
	struct timespec ts = { .tv_sec = ms / 1000,
			       .tv_nsec = (ms % 1000) * 1000000 };
	UNWRAP(nanosleep(&ts, NULL));
}

#endif // !NIMBLE_H
