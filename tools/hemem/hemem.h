#ifndef HEMEM_H
#define HEMEM_H

#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <pthread.h>

#include "pebs.h"
#include "uthash.h"
#include "fifo.h"
#include "envconst.h"
#include "param.h"

//#define HEMEM_DEBUG
#define STATS_THREAD

#define pagefault(...) pebs_pagefault(__VA_ARGS__)
#define paging_init(...) pebs_init(__VA_ARGS__)
#define mmgr_remove(...) pebs_remove_page(__VA_ARGS__)
#define mmgr_stats(...) pebs_stats(__VA_ARGS__)
#define policy_shutdown(...) pebs_shutdown(__VA_ARGS__)

extern volatile bool should_exit;
// extern uint64_t cr3;
extern int dramfd;
extern int nvmfd;
extern int devmemfd;
extern uint64_t missing_faults_handled;
extern uint64_t migrations_up;
extern uint64_t migrations_down;
extern __thread bool internal_malloc;
extern __thread bool old_internal_call;
extern __thread bool internal_call;
extern __thread bool internal_munmap;
extern void *devmem_mmap;

enum memtypes {
	FASTMEM = 0,
	SLOWMEM = 1,
	NMEMTYPES,
};

enum pagetypes { HUGEP = 0, BASEP = 1, NPAGETYPES };

struct hemem_page {
	uint64_t va;
	uint64_t devdax_offset;
	bool in_dram;
	enum pagetypes pt;
	volatile bool migrating;
	bool present;
	// bool written;
	bool hot;
	// uint64_t naccesses;
	uint64_t migrations_up, migrations_down;
	uint64_t local_clock;
	bool ring_present;
	uint64_t accesses[NPBUFTYPES];
	pthread_mutex_t page_lock;

	UT_hash_handle hh;
	struct hemem_page *next, *prev;
	struct fifo_list *list;
};

static inline uint64_t pt_to_pagesize(enum pagetypes pt)
{
	switch (pt) {
	case HUGEP:
		return HUGEPAGE_SIZE;
	case BASEP:
		return BASEPAGE_SIZE;
	default:
		assert(!"Unknown page type");
	}
}

static inline enum pagetypes pagesize_to_pt(uint64_t pagesize)
{
	switch (pagesize) {
	case BASEPAGE_SIZE:
		return BASEP;
	case HUGEPAGE_SIZE:
		return HUGEP;
	default:
		assert(!"Unknown page ssize");
	}
}

void log_init();
void hemem_init();
void hemem_stop();
void *hemem_mmap(void *addr, size_t length, int prot, int flags, int fd,
		 off_t offset);
int hemem_munmap(void *addr, size_t length);
void *handle_fault(void *);
void hemem_migrate(struct hemem_page *page, uint64_t dst_offset, bool to_dram);
void hemem_wp_page(struct hemem_page *page, bool protect);
void hemem_promote_pages(uint64_t addr);
void hemem_demote_pages(uint64_t addr);

#ifdef ALLOC_LRU
void hemem_clear_bits(struct hemem_page *page);
uint64_t hemem_get_bits(struct hemem_page *page);
void hemem_tlb_shootdown(uint64_t va);
#endif

struct hemem_page *get_hemem_page(uint64_t va);

void hemem_print_stats();
void hemem_clear_stats();

// void hemem_start_timing(void);
// void hemem_stop_timing(void);

#define max(a, b)                       \
	({                              \
		__typeof__(a) _a = (a); \
		__typeof__(b) _b = (b); \
		_a > _b ? _a : _b;      \
	})
#define min(a, b)                       \
	({                              \
		__typeof__(a) _a = (a); \
		__typeof__(b) _b = (b); \
		_a < _b ? _a : _b;      \
	})

#endif /* HEMEM_H */
