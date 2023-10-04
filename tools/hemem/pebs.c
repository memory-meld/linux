#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sched.h>
#include <syscall.h>
#include <unistd.h>
#include <x86intrin.h>

#include "interpose.h"
#include "param.h"
#include "unwrap.h"
#include "hemem.h"
#include "pebs.h"
#include "log.h"
#include "timer.h"
#include "spsc-ring.h"

static volatile unsigned long sample_collected = 0, sample_collection_cost = 0;

static struct fifo_list dram_hot_list;
static struct fifo_list dram_cold_list;
static struct fifo_list nvm_hot_list;
static struct fifo_list nvm_cold_list;
static struct fifo_list dram_free_list;
static struct fifo_list nvm_free_list;
static ring_handle_t hot_ring;
static ring_handle_t cold_ring;
static ring_handle_t free_page_ring;
static pthread_mutex_t free_page_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t global_clock = 0;

uint64_t hemem_pages_cnt = 0;
uint64_t other_pages_cnt = 0;
uint64_t total_pages_cnt = 0;
uint64_t zero_pages_cnt = 0;
uint64_t throttle_cnt = 0;
uint64_t unthrottle_cnt = 0;
uint64_t lost_cnt = 0;
uint64_t cools = 0;
uint64_t stat_period_sample_cnt[NPBUFTYPES] = {};

static struct perf_event_mmap_page **perf_page;
static struct perf_event_mmap_page **perf_page_at(ulong i, enum pbuftype j)
{
	return &perf_page[i * NPBUFTYPES + j];
}
static int *pfd;
int *pfd_at(ulong i, enum pbuftype j)
{
	return &pfd[i * NPBUFTYPES + j];
}

volatile bool need_cool_dram = false;
volatile bool need_cool_nvm = false;

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
			    int cpu, int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd,
		       flags);
}

static struct perf_event_mmap_page *perf_setup(__u64 config, __u64 config1,
					       int cpu, enum pbuftype type)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_RAW,
		.size = sizeof(struct perf_event_attr),
		.config = config,
		.config1 = config1,
		.sample_period = SAMPLE_PERIOD(),
		.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID |
			       PERF_SAMPLE_WEIGHT | PERF_SAMPLE_ADDR,
		.disabled = 0,
		.exclude_kernel = 1,
		.exclude_hv = 1,
		.exclude_callchain_kernel = 1,
		.exclude_callchain_user = 1,
		.precise_ip = 3,
	};
	int *fd = pfd_at(cpu, type);
	*fd = UNWRAP(perf_event_open(&attr, -1, cpu, -1, 0));
	LOG_ALWAYS(
		"perf_event_open(type=0x%x,config=0x%llx,config1=0x%llx,period=0x%llx,cpu=%d) = %d\n",
		attr.type, attr.config, attr.config1, attr.sample_period, cpu,
		*fd);

	assert(sizeof(struct perf_sample) <
	       sysconf(_SC_PAGESIZE) * (PERF_PAGES() - 1));
	return UNWRAP(mmap(NULL, sysconf(_SC_PAGESIZE) * PERF_PAGES(),
			   PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0));
}

static void send_hot_request(struct hemem_page *page)
{
	// spsc queue, no need protection
	page->ring_present = true;
	ring_buf_put(hot_ring, page);
}

static void send_cold_request(struct hemem_page *page)
{
	page->ring_present = true;
	ring_buf_put(cold_ring, page);
}

static void handle_perf_sample(struct perf_sample *ps, enum pbuftype j)
{
	switch (ps->header.type) {
	case PERF_RECORD_SAMPLE:
		stat_period_sample_cnt[j]++;
		sample_collected++;
		assert(ps != NULL);
		if (!ps->addr) {
			zero_pages_cnt++;
			break;
		}
		__u64 pfn = ps->addr & HUGE_PFN_MASK;
		struct hemem_page *page = get_hemem_page(pfn);
		total_pages_cnt++;
		if (!page) {
			other_pages_cnt++;
			break;
		}
		hemem_pages_cnt++;
		if (!page->va) {
			break;
		}
		page->accesses[j]++;
		bool hot_write = page->accesses[WRITE] >= HOT_WRITE_THRESHOLD();
		bool hot_read =
			page->accesses[DRAMREAD] + page->accesses[NVMREAD] >=
			HOT_READ_THRESHOLD();
		if (hot_write || hot_read) {
			if (!page->hot || !page->ring_present) {
				send_hot_request(page);
			}
		} else {
			if (page->hot || !page->ring_present) {
				send_cold_request(page);
			}
		}

		page->accesses[j] >>= (global_clock - page->local_clock);
		page->local_clock = global_clock;
		if (page->accesses[j] > PEBS_COOLING_THRESHOLD()) {
			global_clock++;
			need_cool_dram = true;
			need_cool_nvm = true;
		}
		break;

	case PERF_RECORD_THROTTLE:
		throttle_cnt++;
		break;

	case PERF_RECORD_UNTHROTTLE:
		unthrottle_cnt++;
		break;

	case PERF_RECORD_LOST:
		lost_cnt++;
		break;

	default:
		LOG("%s: unknown sample type %u\n", __func__, ps->header.type);
		break;
	}
}

// return true indicate the read is success, otherwise there is nothing to read
// this function handles the corner case of a sample spanning across ring boundary
static bool pebs_sample_read(struct perf_event_mmap_page *p, void *dest,
			     ulong len)
{
	// This points to the head of the data section.  The value
	// continuously increases, it does not wrap.  The value needs
	// to be manually wrapped by the size of the mmap buffer
	// before accessing the samples.
	// On SMP-capable platforms, after reading the data_head
	// value, user space should issue an rmb().
	ulong data_head = p->data_head;
	__sync_synchronize();
	if (data_head <= p->data_tail) {
		return false;
	}
	void *begin = (void *)p + p->data_offset;
	void *end = begin + p->data_size;
	void *head = begin + (p->data_tail % p->data_size);
	ulong remains = end - head;
	if (remains >= len) {
		memcpy(dest, head, len);
	} else {
		memcpy(dest, head, remains);
		memcpy(dest + remains, begin, len - remains);
	}
	ulong size = ((struct perf_event_header *)dest)->size;
	p->data_tail += size;
	// the stack might be corrupted if the sample is too large
	assert(size <= len);
	return true;
}

static void *pebs_scan_thread(void *)
{
	sample_collected = 0;
	sample_collection_cost = __rdtsc();

	pthread_t thread = pthread_self();
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(SCANNING_THREAD_CPU(), &cpuset);
	UNWRAP_ERR(pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset));

	for (uint64_t z = 0; !should_exit; ++z) {
		for (uint64_t i = 0; i < PEBS_NPROCS(); i++) {
			for (uint64_t j = 0; j < NPBUFTYPES; j++) {
				struct perf_event_mmap_page *p =
					*perf_page_at(i, j);
				// LOG("perf page %p buf %p\n", p, pbuf);
				struct perf_sample sample;
				if (!pebs_sample_read(
					    p, &sample,
					    sizeof(struct perf_sample))) {
					continue;
				}
				handle_perf_sample(&sample, j);
			}
		}
		if (!(z % (16L << 20)))
			pebs_stats();
	}
	LOG_ALWAYS("sample_collected %lu\nsample_collection_cost %llu\n",
		   sample_collected, __rdtsc() - sample_collection_cost);

	return NULL;
}

static void pebs_migrate(struct hemem_page *page, uint64_t dst_offset,
			 bool to_dram)
{
	LOG("%s(page=%p,dst_offset=0x%lx,to_dram=%d)\n", __func__, page,
	    dst_offset, to_dram);
	struct timeval start, end;
	gettimeofday(&start, NULL);
	page->migrating = true;
	hemem_wp_page(page, true);
	hemem_migrate(page, dst_offset, to_dram);
	page->migrating = false;
	gettimeofday(&end, NULL);
	LOG_TIME("%s: took %f s\n", __func__, elapsed(&start, &end));
}

// moves page to hot list -- called by migrate thread
static void make_hot(struct hemem_page *page)
{
	assert(page != NULL);
	assert(page->va != 0);

	if (page->hot) {
		if (page->in_dram) {
			assert(page->list == &dram_hot_list);
		} else {
			assert(page->list == &nvm_hot_list);
		}

		return;
	}

	if (page->in_dram) {
		assert(page->list == &dram_cold_list);
		page_list_remove_page(&dram_cold_list, page);
		page->hot = true;
		enqueue_fifo(&dram_hot_list, page);
	} else {
		assert(page->list == &nvm_cold_list);
		page_list_remove_page(&nvm_cold_list, page);
		page->hot = true;
		enqueue_fifo(&nvm_hot_list, page);
	}
}

// moves page to cold list -- called by migrate thread
static void make_cold(struct hemem_page *page)
{
	assert(page != NULL);
	assert(page->va != 0);

	if (!page->hot) {
		if (page->in_dram) {
			assert(page->list == &dram_cold_list);
		} else {
			assert(page->list == &nvm_cold_list);
		}

		return;
	}

	if (page->in_dram) {
		assert(page->list == &dram_hot_list);
		page_list_remove_page(&dram_hot_list, page);
		page->hot = false;
		enqueue_fifo(&dram_cold_list, page);
	} else {
		assert(page->list == &nvm_hot_list);
		page_list_remove_page(&nvm_hot_list, page);
		page->hot = false;
		enqueue_fifo(&nvm_cold_list, page);
	}
}

static struct hemem_page *partial_cool_peek_and_move(struct fifo_list *hot,
						     struct fifo_list *cold,
						     bool dram,
						     struct hemem_page *current)
{
	uint64_t tmp_accesses[NPBUFTYPES];
	static struct hemem_page *start_dram_page = NULL;
	static struct hemem_page *start_nvm_page = NULL;

	if (dram && !need_cool_dram) {
		return current;
	}
	if (!dram && !need_cool_nvm) {
		return current;
	}

	if (start_dram_page == NULL && dram) {
		start_dram_page = hot->last;
	}

	if (start_nvm_page == NULL && !dram) {
		start_nvm_page = hot->last;
	}

	for (unsigned long i = 0; i < COOLING_PAGES(); i++) {
		struct hemem_page *p = next_page(hot, current);
		if (p == NULL) {
			break;
		}
		assert(dram ^ !p->in_dram);

		for (int j = 0; j < NPBUFTYPES; j++) {
			tmp_accesses[j] = p->accesses[j] >>
					  (global_clock - p->local_clock);
		}

		if ((tmp_accesses[WRITE] < HOT_WRITE_THRESHOLD()) &&
		    (tmp_accesses[DRAMREAD] + tmp_accesses[NVMREAD] <
		     HOT_READ_THRESHOLD())) {
			p->hot = false;
		}

		if (dram && (p == start_dram_page)) {
			start_dram_page = NULL;
			need_cool_dram = false;
		}

		if (!dram && (p == start_nvm_page)) {
			start_nvm_page = NULL;
			need_cool_nvm = false;
		}

		if (!p->hot) {
			current = p->next;
			page_list_remove_page(hot, p);
			enqueue_fifo(cold, p);
		} else {
			current = p;
		}
	}

	return current;
}

static void update_page_cool_cursor(struct hemem_page **dram_cursor,
				    struct hemem_page **nvm_cursor,
				    struct hemem_page *page)
{
	if (page == NULL) {
		return;
	}

	if (page == *dram_cursor) {
		assert(page->list == &dram_hot_list);
		*dram_cursor = next_page(page->list, page);
	}
	if (page == *nvm_cursor) {
		assert(page->list == &nvm_hot_list);
		*nvm_cursor = next_page(page->list, page);
	}
}

static struct hemem_page *recv_hot_request()
{
	struct hemem_page *page = ring_buf_get(hot_ring);
	if (page != NULL)
		page->ring_present = false;
	return page;
}

static struct hemem_page *recv_cold_request()
{
	struct hemem_page *page = ring_buf_get(cold_ring);
	if (page != NULL)
		page->ring_present = false;
	return page;
}

static struct hemem_page *recv_free_request()
{
	return ring_buf_get(free_page_ring);
}

static void *pebs_policy_thread(void *)
{
	struct hemem_page *p;
	struct hemem_page *cp;
	struct hemem_page *np;
	struct hemem_page *page = NULL;
	struct hemem_page *dram_cool_cursor = NULL;
	struct hemem_page *nvm_cool_cursor = NULL;

	pthread_t thread = pthread_self();
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(MIGRATION_THREAD_CPU(), &cpuset);
	UNWRAP_ERR(pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset));

	for (;;) {
		while (!ring_buf_empty(free_page_ring)) {
			page = recv_free_request();
			if (page == NULL) {
				continue;
			}

			struct fifo_list *list = page->list;
			assert(list != NULL);
			update_page_cool_cursor(&dram_cool_cursor,
						&nvm_cool_cursor, page);
			page_list_remove_page(list, page);
			if (page->in_dram) {
				enqueue_fifo(&dram_free_list, page);
			} else {
				enqueue_fifo(&nvm_free_list, page);
			}
		}

		for (uint64_t num_ring_reqs = 0;
		     !ring_buf_empty(hot_ring) &&
		     num_ring_reqs < HOT_RING_REQS_THRESHOLD();
		     num_ring_reqs++) {
			page = recv_hot_request();
			if (page == NULL) {
				continue;
			}

			update_page_cool_cursor(&dram_cool_cursor,
						&nvm_cool_cursor, page);
			make_hot(page);
			//printf("hot ring, hot pages:%llu\n", num_ring_reqs);
		}

		for (uint64_t num_ring_reqs = 0;
		     !ring_buf_empty(cold_ring) &&
		     num_ring_reqs < COLD_RING_REQS_THRESHOLD();
		     ++num_ring_reqs) {
			page = recv_cold_request();
			if (page == NULL) {
				continue;
			}

			update_page_cool_cursor(&dram_cool_cursor,
						&nvm_cool_cursor, page);
			make_cold(page);
			//printf("cold ring, cold pages:%llu\n", num_ring_reqs);
		}

		// move each hot NVM page to DRAM
		for (uint64_t migrated_bytes = 0;
		     migrated_bytes < PEBS_KSWAPD_MIGRATE_RATE_BYTE();) {
			p = dequeue_fifo(&nvm_hot_list);
			if (p == NULL) {
				// nothing in NVM is currently hot -- bail out
				break;
			}

			update_page_cool_cursor(&dram_cool_cursor,
						&nvm_cool_cursor, page);

			if ((p->accesses[WRITE] < HOT_WRITE_THRESHOLD()) &&
			    (p->accesses[DRAMREAD] + p->accesses[NVMREAD] <
			     HOT_READ_THRESHOLD())) {
				// it has been cooled, need to move it into the cold list
				p->hot = false;
				enqueue_fifo(&nvm_cold_list, p);
				continue;
			}

			for (int tries = 0; tries < 2; tries++) {
				// find a free DRAM page
				np = dequeue_fifo(&dram_free_list);

				if (np != NULL) {
					assert(!(np->present));

					LOG("%lx: cold %lu -> hot %lu\t slowmem.hot: %lu, slowmem.cold: %lu\t fastmem.hot: %lu, fastmem.cold: %lu\n",
					    p->va, p->devdax_offset,
					    np->devdax_offset,
					    nvm_hot_list.numentries,
					    nvm_cold_list.numentries,
					    dram_hot_list.numentries,
					    dram_cold_list.numentries);

					uint64_t old_offset = p->devdax_offset;
					pebs_migrate(p, np->devdax_offset,
						     true);
					np->devdax_offset = old_offset;
					np->in_dram = false;
					np->present = false;
					np->hot = false;
					for (int i = 0; i < NPBUFTYPES; i++) {
						np->accesses[i] = 0;
					}

					enqueue_fifo(&dram_hot_list, p);
					enqueue_fifo(&nvm_free_list, np);

					migrated_bytes += pt_to_pagesize(p->pt);
					break;
				}
				assert(np == NULL);

				// no free dram page, try to find a cold dram page to move down
				cp = dequeue_fifo(&dram_cold_list);
				if (cp == NULL) {
					// all dram pages are hot, so put it back in list we got it from
					enqueue_fifo(&nvm_hot_list, p);
					goto out;
				}
				assert(cp != NULL);

				// find a free nvm page to move the cold dram page to
				np = dequeue_fifo(&nvm_free_list);
				if (np != NULL) {
					assert(!(np->present));

					LOG("%lx: hot %lu -> cold %lu\t slowmem.hot: %lu, slowmem.cold: %lu\t fastmem.hot: %lu, fastmem.cold: %lu\n",
					    cp->va, cp->devdax_offset,
					    np->devdax_offset,
					    nvm_hot_list.numentries,
					    nvm_cold_list.numentries,
					    dram_hot_list.numentries,
					    dram_cold_list.numentries);

					uint64_t old_offset = cp->devdax_offset;
					pebs_migrate(cp, np->devdax_offset,
						     false);
					np->devdax_offset = old_offset;
					np->in_dram = true;
					np->present = false;
					np->hot = false;
					for (int i = 0; i < NPBUFTYPES; i++) {
						np->accesses[i] = 0;
					}

					enqueue_fifo(&nvm_cold_list, cp);
					enqueue_fifo(&dram_free_list, np);
				}
				// else {
				//          LOG_ALWAYS("OOM: slowmem.hot: %lu, slowmem.cold: %lu, slowmem.free: %lu\t fastmem.hot: %lu, fastmem.cold: %lu, fastmem.free: %lu\n",
				//                     nvm_hot_list.numentries, nvm_cold_list.numentries, nvm_free_list.numentries, dram_hot_list.numentries, dram_cold_list.numentries, dram_free_list.numentries);
				// }
				assert(np != NULL);
			}
		}

		dram_cool_cursor = partial_cool_peek_and_move(&dram_hot_list,
							      &dram_cold_list,
							      true,
							      dram_cool_cursor);
		nvm_cool_cursor = partial_cool_peek_and_move(
			&nvm_hot_list, &nvm_cold_list, false, nvm_cool_cursor);

out:
		// LOG_TIME("migrate: %f s\n", elapsed(&start, &end));
	}

	return NULL;
}

static struct hemem_page *pebs_allocate_page()
{
	struct timeval start, end;

	gettimeofday(&start, NULL);

	struct hemem_page *page = dequeue_fifo(&dram_free_list);
	if (page != NULL) {
		assert(page->in_dram);
		assert(!page->present);

		page->present = true;
		enqueue_fifo(&dram_cold_list, page);

		gettimeofday(&end, NULL);
		LOG_TIME("mem_policy_allocate_page: %f s\n",
			 elapsed(&start, &end));

		return page;
	}

	// DRAM is full, fall back to NVM
	page = dequeue_fifo(&nvm_free_list);
	if (page != NULL) {
		assert(!page->in_dram);
		assert(!page->present);

		page->present = true;
		enqueue_fifo(&nvm_cold_list, page);

		gettimeofday(&end, NULL);
		LOG_TIME("mem_policy_allocate_page: %f s\n",
			 elapsed(&start, &end));

		return page;
	}

	assert(!"Out of memory");
}

struct hemem_page *pebs_pagefault(void)
{
	// do the heavy lifting of finding the devdax file offset to place the page
	struct hemem_page *page = pebs_allocate_page();
	assert(page != NULL);
	return page;
}

static void send_free_request(struct hemem_page *page)
{
	// this is a mpsc queue, sender side needs to be protected
	pthread_mutex_lock(&free_page_ring_lock);
	while (ring_buf_full(free_page_ring))
		;
	ring_buf_put(free_page_ring, page);
	pthread_mutex_unlock(&free_page_ring_lock);
}

void pebs_remove_page(struct hemem_page *page)
{
	assert(page != NULL);

	LOG("pebs: remove page, put this page into free_page_ring: va: 0x%lx\n",
	    page->va);
	send_free_request(page);

	page->present = false;
	page->hot = false;
	for (int i = 0; i < NPBUFTYPES; i++) {
		page->accesses[i] = 0;
	}
}

static void hemem_page_init(struct hemem_page *p, bool in_dram,
			    uint64_t devdax_offset)
{
	struct hemem_page new = {
		.devdax_offset = devdax_offset,
		.present = false,
		.in_dram = in_dram,
		.ring_present = false,
		.pt = pagesize_to_pt(PAGE_SIZE),
	};
	*p = new;
	UNWRAP_ERR(pthread_mutex_init(&(p->page_lock), NULL));
}

void pebs_init(void)
{
	LOG("pebs_init: started\n");

	perf_page = calloc(PEBS_NPROCS() * NPBUFTYPES,
			   sizeof(struct perf_event_mmap_page *));
	pfd = calloc(PEBS_NPROCS() * NPBUFTYPES, sizeof(int));
	for (ulong i = 0; i < PEBS_NPROCS(); i++) {
		// MEM_LOAD_L3_MISS_RETIRED.LOCAL_DRAM
		*perf_page_at(i, DRAMREAD) = perf_setup(0x1d3, 0, i, DRAMREAD);
		// MEM_LOAD_RETIRED.LOCAL_PMM
		*perf_page_at(i, NVMREAD) = perf_setup(0x80d1, 0, i, NVMREAD);
		// MEM_INST_RETIRED.ALL_STORES
		*perf_page_at(i, WRITE) = perf_setup(0x82d0, 0, i, WRITE);

		// MEM_TRANS_RETIRED.LOAD_LATENCY_GT_4
		//*perf_page_at(i, READ) = perf_setup(0x1cd, 0x4, i);
		// MEM_INST_RETIRED.ALL_LOADS
		//*perf_page_at(i, READ) = perf_setup(0x81d0, 0, i);
		// MEM_INST_RETIRED.STLB_MISS_STORES
		//*perf_page_at(i, WRITE) = perf_setup(0x12d0, 0, i);
	}

	UNWRAP_ERR(pthread_mutex_init(&(dram_free_list.list_lock), NULL));
	for (uint64_t i = 0; i < DRAMSIZE() / PAGE_SIZE; i++) {
		struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
		hemem_page_init(p, true, i * PAGE_SIZE);
		enqueue_fifo(&dram_free_list, p);
	}
	LOG("dram free list allocation complete\n");

	UNWRAP_ERR(pthread_mutex_init(&(nvm_free_list.list_lock), NULL));
	for (uint64_t i = 0; i < NVMSIZE() / PAGE_SIZE; i++) {
		struct hemem_page *p = calloc(1, sizeof(struct hemem_page));
		hemem_page_init(p, false, i * PAGE_SIZE);
		enqueue_fifo(&nvm_free_list, p);
	}
	LOG("nvm free list allocation complete\n");

	UNWRAP_ERR(pthread_mutex_init(&(dram_hot_list.list_lock), NULL));
	UNWRAP_ERR(pthread_mutex_init(&(dram_cold_list.list_lock), NULL));
	UNWRAP_ERR(pthread_mutex_init(&(nvm_hot_list.list_lock), NULL));
	UNWRAP_ERR(pthread_mutex_init(&(nvm_cold_list.list_lock), NULL));

	{
		hot_ring = ring_buf_init(calloc(CAPACITY(),
						sizeof(ring_buf_elem_t)),
					 CAPACITY());
		cold_ring = ring_buf_init(calloc(CAPACITY(),
						 sizeof(ring_buf_elem_t)),
					  CAPACITY());
		free_page_ring = ring_buf_init(calloc(CAPACITY(),
						      sizeof(ring_buf_elem_t)),
					       CAPACITY());
	}
	{
		pthread_t scan_thread;
		UNWRAP_ERR(pthread_create(&scan_thread, NULL, pebs_scan_thread,
					  NULL));
		UNWRAP_ERR(pthread_setname_np(scan_thread, "pebs_thread"));
	}
	{
		pthread_t kswapd_thread;
		UNWRAP_ERR(pthread_create(&kswapd_thread, NULL,
					  pebs_policy_thread, NULL));
		UNWRAP_ERR(pthread_setname_np(kswapd_thread, "policy_thread"));
	}

	LOG("Memory management policy is PEBS\n");

	LOG("pebs_init: finished\n");
}

void dump_list_element_va(struct hemem_page *p)
{
	LOG_STATS(" 0x%lx", p->va);
}

void pebs_shutdown()
{
	should_exit = 1;
	for (ulong i = 0; i < PEBS_NPROCS(); i++) {
		for (int j = 0; j < NPBUFTYPES; j++) {
			UNWRAP(ioctl(*pfd_at(i, j), PERF_EVENT_IOC_DISABLE, 0));
			// UNWRAP_ERR(real_munmap(perf_page_at(i, j),
			// 		       sysconf(_SC_PAGESIZE) *
			// 			       PERF_PAGES()));
		}
	}
	// free(pfd);
	// free(perf_page);
	sleep(5);

	LOG_ALWAYS("pebs_shutdown\n");
}

void pebs_dump_lists()
{
	LOG_STATS("%s: dram_hot_list va:", __func__);
	ulong len = fifo_for_each(&dram_hot_list, dump_list_element_va);
	LOG_STATS("\n%s: dram_hot_list len %lu\n", __func__, len);

	LOG_STATS("%s: nvm_hot_list va:", __func__);
	len = fifo_for_each(&nvm_hot_list, dump_list_element_va);
	LOG_STATS("\n%s: nvm_hot_list len %lu\n", __func__, len);

	LOG_STATS("%s: dram_cold_list va:", __func__);
	len = fifo_for_each(&dram_cold_list, dump_list_element_va);
	LOG_STATS("\n%s: dram_cold_list len %lu\n", __func__, len);

	LOG_STATS("%s: nvm_cold_list va:", __func__);
	len = fifo_for_each(&nvm_cold_list, dump_list_element_va);
	LOG_STATS("\n%s: nvm_cold_list len %lu\n", __func__, len);
}

void pebs_stats()
{
	uint64_t total_samples = stat_period_sample_cnt[DRAMREAD] +
				 stat_period_sample_cnt[NVMREAD] +
				 stat_period_sample_cnt[WRITE];
	LOG_STATS("\tdram_hot_list.numentries: [%ld]"
		  "\tdram_cold_list.numentries: [%ld]"
		  "\tdram_free_list.numentries: [%ld]"
		  "\tnvm_hot_list.numentries: [%ld]"
		  "\tnvm_cold_list.numentries: [%ld]"
		  "\tnvm_free_list.numentries: [%ld]"
		  "\themem_pages: [%lu]"
		  "\ttotal_pages: [%lu]"
		  "\tzero_pages: [%ld]"
		  "\tthrottle/unthrottle_cnt: [%ld/%ld]"
		  "\tcools: [%ld]"
		  "\tdramread: [%ld/%ld]"
		  "\tnvmread: [%ld/%ld]"
		  "\twrite: [%ld/%ld]"
		  "\n",
		  dram_hot_list.numentries, dram_cold_list.numentries,
		  dram_free_list.numentries, nvm_hot_list.numentries,
		  nvm_cold_list.numentries, nvm_free_list.numentries,
		  hemem_pages_cnt, total_pages_cnt, zero_pages_cnt,
		  throttle_cnt, unthrottle_cnt, cools,
		  stat_period_sample_cnt[DRAMREAD], total_samples,
		  stat_period_sample_cnt[NVMREAD], total_samples,
		  stat_period_sample_cnt[WRITE], total_samples);
	stat_period_sample_cnt[DRAMREAD] = stat_period_sample_cnt[NVMREAD] =
		stat_period_sample_cnt[WRITE] = 0;
	hemem_pages_cnt = total_pages_cnt = throttle_cnt = unthrottle_cnt = 0;
}
