#define _GNU_SOURCE
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <sched.h>
#include <syscall.h>
#include <unistd.h>

#include "log.h"
#include "hemem.h"
#include "param.h"
#include "unwrap.h"
#include "timer.h"
#include "uthash.h"
#include "pebs.h"
#include "userfaultfd.h"
#include "interpose.h"

#ifdef ALLOC_LRU
#include "policies/paging.h"
#endif
volatile bool should_exit = false;

pthread_t fault_thread;

int dramfd = -1;
int nvmfd = -1;
int devmemfd = -1;
long uffd = -1;

bool timing = false;

uint64_t mem_mmaped = 0;
uint64_t mem_allocated = 0;
uint64_t pages_allocated = 0;
uint64_t pages_freed = 0;
uint64_t fastmem_allocated = 0;
uint64_t slowmem_allocated = 0;
uint64_t wp_faults_handled = 0;
uint64_t missing_faults_handled = 0;
uint64_t migrations_up = 0;
uint64_t migrations_down = 0;
uint64_t bytes_migrated = 0;
uint64_t memcpys = 0;
uint64_t memsets = 0;
uint64_t migration_waits = 0;

// static bool cr3_set = false;
// uint64_t cr3 = 0;

#ifndef USE_DMA
pthread_t copy_threads[MAX_COPY_THREADS];
#endif
pthread_t stats_thread;

struct hemem_page *pages = NULL;
pthread_mutex_t pages_lock = PTHREAD_MUTEX_INITIALIZER;

void *dram_devdax_mmap;
void *nvm_devdax_mmap;
//void *devmem_mmap;

#ifndef USE_DMA
struct pmemcpy {
	pthread_mutex_t lock;
	pthread_barrier_t barrier;
	_Atomic bool write_zeros;
	_Atomic(void *) dst;
	_Atomic(void *) src;
	_Atomic size_t length;
};

static struct pmemcpy pmemcpy;

void *hemem_parallel_memcpy_thread(void *arg)
{
	uint64_t tid = (uint64_t)arg;
	LOG("%s: thread %lu started\n", __func__, tid);

	for (;;) {
		// LOG("hemem_parallel_memcpy_thread %lu wait for job\n", tid);
		int r = pthread_barrier_wait(&pmemcpy.barrier);
		assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
		if (tid == 0) {
			memcpys++;
		}

		size_t length = pmemcpy.length;
		size_t chunk_size = length / MAX_COPY_THREADS;
		void *dst = pmemcpy.dst + (tid * chunk_size);
		if (!pmemcpy.write_zeros) {
			void *src = pmemcpy.src + (tid * chunk_size);
			LOG("%s: %lu got job %p->%p len 0x%lx\n", __func__, tid,
			    src, dst, length);
			memcpy(dst, src, chunk_size);
		} else {
			LOG("%s: %lu got job zero fill %p len 0x%lx\n",
			    __func__, tid, dst, length);
			memset(dst, 0, chunk_size);
		}
		LOG("%s: thread %lu done\n", __func__, tid);

		r = pthread_barrier_wait(&pmemcpy.barrier);
		assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
	}

	return NULL;
}
#endif

#ifdef STATS_THREAD
static void *hemem_stats_thread(void *)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(STATS_THREAD_CPU(), &cpuset);
	pthread_t thread = pthread_self();
	UNWRAP_ERR(pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset));

	for (; !should_exit;) {
		sleep(1);

		hemem_print_stats();
		hemem_clear_stats();
	}
	return NULL;
}
#endif

void add_page(struct hemem_page *page)
{
	struct hemem_page *p;
	LOG("%s: adding page va %p\n", __func__, (void *)page->va);
	UNWRAP_ERR(pthread_mutex_lock(&pages_lock));
	HASH_FIND(hh, pages, &(page->va), sizeof(uint64_t), p);
	assert(p == NULL);
	HASH_ADD(hh, pages, va, sizeof(uint64_t), page);
	UNWRAP_ERR(pthread_mutex_unlock(&pages_lock));
}

void remove_page(struct hemem_page *page)
{
	UNWRAP_ERR(pthread_mutex_lock(&pages_lock));
	HASH_DEL(pages, page);
	UNWRAP_ERR(pthread_mutex_unlock(&pages_lock));
}

struct hemem_page *find_page(uint64_t va)
{
	struct hemem_page *page;
	HASH_FIND(hh, pages, &va, sizeof(uint64_t), page);
	return page;
}

void *real_address(uint64_t offset, bool in_dram)
{
	if (in_dram) {
		assert(offset < DRAMSIZE());
		assert(offset + HUGEPAGE_SIZE <= DRAMSIZE());
		return dram_devdax_mmap + offset;
	} else {
		assert(offset < NVMSIZE());
		assert(offset + HUGEPAGE_SIZE <= NVMSIZE());
		return nvm_devdax_mmap + offset;
	}
}

void hemem_init()
{
	errno = 0;

	LOG("hemem_init: started\n");

	dramfd = UNWRAP(open(DRAMPATH(), O_RDWR));

	nvmfd = UNWRAP(open(NVMPATH(), O_RDWR));

	// devmemfd = UNWRAP(open("/dev/mem", O_RDWR | O_SYNC));

	uffd = uffdio_open(O_CLOEXEC | O_NONBLOCK,
			   UFFD_FEATURE_MISSING_SHMEM |
				   UFFD_FEATURE_MISSING_HUGETLBFS |
				   UFFD_FEATURE_PAGEFAULT_FLAG_WP);

	UNWRAP_ERR(pthread_create(&fault_thread, NULL, handle_fault, 0));

	UNWRAP_ERR(pthread_setname_np(fault_thread, "fault_handler"));

	LOG("%s mmaping backing DRAM\n", __func__);
	dram_devdax_mmap =
		UNWRAP_ERR(real_mmap(NULL, DRAMSIZE(), PROT_READ | PROT_WRITE,
				     MAP_SHARED | MAP_POPULATE, dramfd, 0));

	LOG("%s mmaping backing PMEM\n", __func__);
	nvm_devdax_mmap =
		UNWRAP_ERR(real_mmap(NULL, NVMSIZE(), PROT_READ | PROT_WRITE,
				     MAP_SHARED | MAP_POPULATE, nvmfd, 0));

	// LOG("%s mmaping system RAM\n", __func__);
	// devmem_mmap =
	// 	UNWRAP_ERR(real_mmap(NULL, MEMSIZE(), PROT_READ | PROT_WRITE,
	// 			 MAP_SHARED | MAP_POPULATE, devmemfd, 0));

#ifndef USE_DMA
	UNWRAP_ERR(pthread_barrier_init(&pmemcpy.barrier, NULL,
					MAX_COPY_THREADS + 1));
	UNWRAP_ERR(pthread_mutex_init(&pmemcpy.lock, NULL));
	for (uint64_t i = 0; i < MAX_COPY_THREADS; i++) {
		UNWRAP_ERR(pthread_create(&copy_threads[i], NULL,
					  hemem_parallel_memcpy_thread,
					  (void *)i));
		char name[512];
		snprintf(name, 512, "dma_copy_%lu", i);
		UNWRAP_ERR(pthread_setname_np(copy_threads[i], name));
	}
#endif

#ifdef STATS_THREAD
	UNWRAP_ERR(
		pthread_create(&stats_thread, NULL, hemem_stats_thread, NULL));
	UNWRAP_ERR(pthread_setname_np(stats_thread, "stats_thread"));
#endif

	paging_init();

	struct hemem_page *dummy_page = calloc(1, sizeof(struct hemem_page));
	add_page(dummy_page);

#ifdef USE_DMA
	struct uffdio_dma_channs uffdio_dma_channs = {
		.num_channs = NUM_CHANNS(),
		.size_per_dma_request = SIZE_PER_DMA_REQUEST
	};
	UNWRAP(ioctl(uffd, UFFDIO_DMA_REQUEST_CHANNS, &uffdio_dma_channs));
#endif

	LOG("hemem_init: finished\n");
}

void hemem_stop()
{
#ifdef USE_DMA
	struct uffdio_dma_channs uffdio_dma_channs = {
		.num_channs = NUM_CHANNS(),
		.size_per_dma_request = SIZE_PER_DMA_REQUEST
	};
	UNWRAP(ioctl(uffd, UFFDIO_DMA_RELEASE_CHANNS, &uffdio_dma_channs));
#endif

	policy_shutdown();
}

#ifndef USE_DMA
static void hemem_parallel_memset(void *addr, int, size_t n)
{
	UNWRAP_ERR(pthread_mutex_lock(&(pmemcpy.lock)));
	pmemcpy.dst = addr;
	pmemcpy.length = n;
	pmemcpy.write_zeros = true;

	int r = pthread_barrier_wait(&pmemcpy.barrier);
	assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
	// LOG("memset job issued\n");

	r = pthread_barrier_wait(&pmemcpy.barrier);
	assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);

	// LOG("memset job completed\n");
	UNWRAP_ERR(pthread_mutex_unlock(&(pmemcpy.lock)));
}
#endif

#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define round_up(x, y) ((((x)-1) | __round_mask(x, y)) + 1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))

static void hemem_mmap_populate(void *addr, size_t length)
{
	LOG("%s(addr=%p,len=0x%lx)\n", __func__, addr, length);
	assert(addr != 0 && length != 0);
	assert((uint64_t)addr % PAGE_SIZE == 0);

	for (uint64_t page_boundry = (uint64_t)addr;
	     page_boundry < (uint64_t)addr + length;) {
		struct hemem_page *page = pagefault();
		assert(page != NULL);
		UNWRAP_ERR(pthread_mutex_init(&(page->page_lock), NULL));
		page->migrating = false;
		page->migrations_up = page->migrations_down = 0;

		// let policy algorithm do most of the heavy lifting of finding a free page
		uint64_t offset = page->devdax_offset;
		bool in_dram = page->in_dram;
		uint64_t pagesize = pt_to_pagesize(page->pt);

		void *addr = real_address(offset, in_dram);
#ifndef USE_DMA
		hemem_parallel_memset(addr, 0, pagesize);
#else
		memset(real_address, 0, pagesize);
#endif
		memsets++;

		// now that we have an offset determined via the policy algorithm, actually map
		// the page for the application
		LOG("%s: allocating backing page page_boundry=%p,dram=%d,offset=0x%lx\n",
		    __func__, (void *)page_boundry, in_dram, offset);
		void *p = UNWRAP_ERR(real_mmap(
			(void *)page_boundry, pagesize, PROT_READ | PROT_WRITE,
			MAP_SHARED | MAP_POPULATE | MAP_FIXED,
			(in_dram ? dramfd : nvmfd), offset));
		assert(p == (void *)page_boundry);

		// use mmap return addr to track new page's virtual address
		page->va = (uint64_t)p;

		// re-register new mmap region with userfaultfd
		UNWRAP(uffdio_register(uffd, p, pagesize));

		mem_allocated += pagesize;
		pages_allocated++;

		// place in hemem's page tracking list
		add_page(page);
		page_boundry += pagesize;
	}
}

// void *hemem_mmap_assign_range(void *addr, size_t length, int prot, int flags,
// 			      int fd, off_t offset)
// {
// 	LOG("%s(addr=%p,len=0x%lx,prot=0x%x,flags=0x%x,fd=%d,offset=0x%lx)\n",
// 	    __func__, addr, length, prot, flags, fd, offset);
// 	// we are merely assigning an address range, real page allocation and
// 	// population is done in hemme_mmap_populate()
// 	void *p = real_mmap(addr, length, prot,
// 			    MAP_ANON | MAP_SHARED | MAP_NORESERVE, dramfd,
// 			    offset);
// 	// try again using pmem
// 	if (p == MAP_FAILED) {
// 		LOG("%s: failed using DRAM, retrying using PMEM\n", __func__);
// 		p = real_mmap(addr, length, prot,
// 			      MAP_ANON | MAP_SHARED | MAP_NORESERVE, nvmfd,
// 			      offset);
// 	}
// 	// still fails, we are out of memory
// 	if (p == MAP_FAILED) {
// 		LOG("%s: DRAM and PMEM both failed\n", __func__);
// 		return MAP_FAILED;
// 	}

// 	// register with uffd
// 	int err = uffdio_register(uffd, p, length);
// 	if (err == -1) {
// 		LOG("%s: failed to register with userfaultfd errno %d `%s`\n",
// 		    __func__, errno, strerror(errno));
// 		real_munmap(addr, length);
// 		return MAP_FAILED;
// 	}

// #ifdef UFFDIO_CR3
// 	if (!cr3_set) {
// 		struct uffdio_cr3 uffdio_cr3;
// 		if (ioctl(uffd, UFFDIO_CR3, &uffdio_cr3) < 0) {
// 			perror("ioctl uffdio_cr3");
// 			assert(0);
// 		}
// 		cr3 = uffdio_cr3.cr3;
// 		cr3_set = true;
// 	}
// #endif

// 	return p;
// }

int hemme_mmap_fix_flags(int flags)
{
	// if (MAP_POPULATE == (flags & MAP_POPULATE)) {
	// 	flags &= ~MAP_POPULATE;
	// 	LOG("hemem_mmap: unset MAP_POPULATE\n");
	// }
	// if (MAP_POPULATE != (flags & MAP_POPULATE)) {
	// 	flags |= ~MAP_POPULATE;
	// 	LOG("hemem_mmap: set MAP_POPULATE\n");
	// }
	if (MAP_PRIVATE == (flags & MAP_PRIVATE)) {
		flags &= ~MAP_PRIVATE;
		flags |= MAP_SHARED;
		LOG("hemem_mmap: MAP_PRIVATE -> MAP_SHARED\n");
	}
	if (MAP_ANON == (flags & MAP_ANON)) {
		flags &= ~MAP_ANON;
		LOG("hemem_mmap: unset MAP_ANON\n");
	}
	if (MAP_HUGETLB == (flags & MAP_HUGETLB)) {
		flags &= ~MAP_HUGETLB;
		LOG("hemem_mmap: unset MAP_HUGETLB\n");
	}
	// if (MAP_SYNC == (flags & MAP_SYNC)) {
	// 	flags &= ~MAP_SYNC;
	// 	LOG("hemem_mmap: unset MAP_SYNC\n");
	// }
	return flags;
}

void *hemem_mmap(void *addr, size_t length, int prot, int flags, int fd,
		 off_t offset)
{
	LOG("%s(addr=%p,len=0x%lx,prot=0x%x,flags=0x%x,fd=%d,offset=0x%lx) DRAMSIZE=0x%lx NVMSIZE=0x%lx\n",
	    __func__, addr, length, prot, flags, fd, offset, DRAMSIZE(),
	    NVMSIZE());

	assert(length != 0 && length % HUGEPAGE_SIZE == 0 && offset == 0);

	if (length > DRAMSIZE() + NVMSIZE() - mem_allocated) {
		errno = ENOMEM;
		return MAP_FAILED;
	}

	// flags = hemme_mmap_fix_flags(flags);

	// try to break down large allocations
	// for (size_t done = 0; done < length;) {
	// 	size_t chunk = min(length - done, SMALLALLOCSIZE());
	// 	// try to allocate from DRAM first
	// 	void *ret = hemem_mmap_assign_range(
	// 		addr + done, chunk, prot,
	// 		flags | (addr ? MAP_FIXED : 0), -1, 0);
	// 	if (ret == MAP_FAILED) {
	// 		// unmmap previously assigned
	// 		for (size_t assigned = 0; assigned < done;) {
	// 			size_t chunk = min(length - assigned,
	// 					   SMALLALLOCSIZE());
	// 			hemem_munmap(addr + assigned, chunk);
	// 			assigned += chunk;
	// 		}
	// 		return MAP_FAILED;
	// 	}
	// 	// set addr to the beginning of the assigned region
	// 	if (!addr) {
	// 		addr = ret;
	// 	}
	// 	done += chunk;
	// }

	// we are merely assigning an address range, real page allocation and
	// population is done in hemme_mmap_populate()
	addr = UNWRAP_ERR(real_mmap(addr, length, PROT_READ | PROT_WRITE,
				    MAP_ANON | MAP_SHARED | MAP_NORESERVE, -1,
				    0));
	// the addr we got now is merely a hint, it might not be aligned
	if ((ulong)addr % PAGE_SIZE) {
		UNWRAP_ERR(real_munmap(addr, length));
		addr = UNWRAP_ERR(real_mmap(
			(void *)round_down((ulong)addr, PAGE_SIZE), length,
			PROT_READ | PROT_WRITE,
			MAP_ANON | MAP_SHARED | MAP_NORESERVE | MAP_FIXED, -1,
			0));
		assert((ulong)addr % PAGE_SIZE == 0);
	}
	UNWRAP(uffdio_register(uffd, addr, length));

	if (MAP_POPULATE == (flags & MAP_POPULATE)) {
		hemem_mmap_populate(addr, length);
	}

	return addr;
}

extern void pebs_dump_lists();
int hemem_munmap(void *addr, size_t length)
{
	if (length > SMALLALLOCSIZE()) {
		pebs_dump_lists();
	}

	// for each page in region specified...
	for (uint64_t page_boundry = (uint64_t)addr;
	     page_boundry < (uint64_t)addr + length;) {
		// find the page in hemem's trackign list
		struct hemem_page *page = find_page(page_boundry);
		if (page != NULL) {
			// remove page form hemem's and policy's list
			remove_page(page);
			mmgr_remove(page);

			mem_allocated -= pt_to_pagesize(page->pt);
			mem_mmaped -= pt_to_pagesize(page->pt);
			pages_freed++;

			// move to next page
			page_boundry += pt_to_pagesize(page->pt);
		} else {
			// TODO: deal with holes?
			//LOG("hemem_mmunmap: no page to umnap\n");
			page_boundry += BASEPAGE_SIZE;
		}
	}

	return real_munmap(addr, length);
}

#ifndef USE_DMA
static void hemem_parallel_memcpy(void *dst, void *src, size_t length)
{
	UNWRAP_ERR(pthread_mutex_lock(&(pmemcpy.lock)));
	pmemcpy.dst = dst;
	pmemcpy.src = src;
	pmemcpy.length = length;
	pmemcpy.write_zeros = false;

	int r = pthread_barrier_wait(&pmemcpy.barrier);
	assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);

	// LOG("%s: memcpy job issued %p->%p len 0x%lx\n", __func__, src, dst, length);

	r = pthread_barrier_wait(&pmemcpy.barrier);
	assert(r == 0 || r == PTHREAD_BARRIER_SERIAL_THREAD);
	// LOG("%s: memcpy job completed\n", __func__);
	UNWRAP_ERR(pthread_mutex_unlock(&(pmemcpy.lock)));
}
#endif

void hemem_migrate(struct hemem_page *page, uint64_t dst_offset, bool to_dram)
{
	LOG("%s(page=%p,dst_offset=0x%lx,to_dram=%d)\n", __func__, page,
	    dst_offset, to_dram);
	assert(page->in_dram ^ to_dram);
	assert(page != NULL);

	struct timeval start, end;
	struct timeval migrate_start;
	gettimeofday(&migrate_start, NULL);

	uint64_t pagesize = pt_to_pagesize(page->pt);
	void *src_addr = real_address(page->devdax_offset, page->in_dram);
	void *dst_addr = real_address(dst_offset, to_dram);

	// copy page from faulting location to temp location
	gettimeofday(&start, NULL);
#ifdef USE_DMA
	struct uffdio_dma_copy uffdio_dma_copy = {
		.src[0] = (uint64_t)src_addr,
		.dst[0] = (uint64_t)dst_addr,
		.len[0] = pagesize,
		.count = 1,
		.mode = 0,
		.copy = 0,
	};
	UNWRAP(ioctl(uffd, UFFDIO_DMA_COPY, &uffdio_dma_copy));
#else
	hemem_parallel_memcpy(dst_addr, src_addr, pagesize);
#endif
	gettimeofday(&end, NULL);
	LOG_TIME("%s: copying took %f s\n", __func__, elapsed(&start, &end));

	gettimeofday(&start, NULL);
	void *va = UNWRAP_ERR(real_mmap((void *)page->va, pagesize,
					PROT_READ | PROT_WRITE,
					MAP_SHARED | MAP_POPULATE | MAP_FIXED,
					to_dram ? dramfd : nvmfd, dst_offset));
	assert(va == (void *)page->va);
	assert(page->va % HUGEPAGE_SIZE == 0);
	gettimeofday(&end, NULL);
	LOG_TIME("%s: mmap took %f s\n", __func__, elapsed(&start, &end));

	// re-register new mmap region with userfaultfd
	gettimeofday(&start, NULL);
	UNWRAP(uffdio_register(uffd, va, pagesize));
	gettimeofday(&end, NULL);
	LOG_TIME("%s: register took %f s\n", __func__, elapsed(&start, &end));

	if (to_dram) {
		page->migrations_up++;
		migrations_up++;
	} else {
		page->migrations_down++;
		migrations_down++;
	}

	page->devdax_offset = dst_offset;
	page->in_dram = to_dram;

	bytes_migrated += pagesize;

	struct timeval migrate_end;
	gettimeofday(&migrate_end, NULL);
	LOG_TIME("%s: took %f s\n", __func__,
		 elapsed(&migrate_start, &migrate_end));
}

void hemem_wp_page(struct hemem_page *page, bool protect)
{
	uint64_t addr = page->va;
	uint64_t pagesize = pt_to_pagesize(page->pt);

	assert(addr != 0);
	assert(addr % HUGEPAGE_SIZE == 0);

	struct timeval start, end;
	gettimeofday(&start, NULL);
	UNWRAP(uffdio_writeprotect(uffd, (void *)addr, pagesize,
				   protect ? UFFDIO_WRITEPROTECT_MODE_WP : 0));
	gettimeofday(&end, NULL);

	LOG_TIME("uffdio_writeprotect: %f s\n", elapsed(&start, &end));
}

void handle_wp_fault(uint64_t page_boundry)
{
	LOG_ALWAYS("%s(page_boundry=0x%lx)\n", __func__, page_boundry);
	struct hemem_page *page = find_page(page_boundry);
	assert(page != NULL);

	migration_waits++;

	LOG("%s: waiting for migration for page %lx\n", __func__, page_boundry);

	while (page->migrating) {
	}
}

void handle_missing_fault(uint64_t page_boundry)
{
	LOG("%s(page_boundry=0x%lx)\n", __func__, page_boundry);

	struct timeval missing_start;
	gettimeofday(&missing_start, NULL);

	hemem_mmap_populate((void *)page_boundry, HUGEPAGE_SIZE);

	missing_faults_handled++;
	struct timeval missing_end;
	gettimeofday(&missing_end, NULL);
	LOG_TIME("%s: took %f s\n", __func__,
		 elapsed(&missing_start, &missing_end));
}

void *handle_fault(void *)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	pthread_t thread = pthread_self();
	CPU_SET(FAULT_THREAD_CPU(), &cpuset);
	UNWRAP_ERR(pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset));

	// we should not use !should_exit as faults sill might be trigger by
	// other threads while shutting down
	for (;;) {
		struct pollfd pollfd = {
			.fd = uffd,
			.events = POLLIN,
		};
		if (!UNWRAP(poll(&pollfd, 1, -1))) {
			break;
		}
		assert(!(pollfd.revents & POLLERR));
		assert(pollfd.revents & POLLIN);
		struct uffd_msg msgbuf[MAX_UFFD_MSGS];
		ssize_t nread =
			UNWRAP(read(uffd, &msgbuf,
				    MAX_UFFD_MSGS * sizeof(struct uffd_msg)));
		if (!nread) {
			break;
		}
		assert(!(nread % sizeof(struct uffd_msg)));
		for (uint64_t total = nread / sizeof(struct uffd_msg), i = 0;
		     i < total; ++i) {
			struct uffd_msg *msg = &msgbuf[i];
			assert(msg->event == UFFD_EVENT_PAGEFAULT);
			uint64_t flags = msg->arg.pagefault.flags;
			uint64_t addr = (uint64_t)msg->arg.pagefault.address;
			ulong len = HUGEPAGE_SIZE;
			uint64_t page_boundry = addr & ~(len - 1);

			// handle WP first
			if (flags & UFFD_PAGEFAULT_FLAG_WP) {
				// write to a migrating page
				LOG("%s: wp fault\n", __func__);
				handle_wp_fault(page_boundry);
			} else if (flags & UFFD_PAGEFAULT_FLAG_WRITE) {
				// write to a backingless page
				LOG("%s: missing fault\n", __func__);
				handle_missing_fault(page_boundry);
			} else {
				LOG("%s: unknown fault 0x%lx\n", __func__,
				    flags);
				assert(0);
			}
			UNWRAP(uffdio_wake(uffd, (void *)page_boundry, len));
		}
	}

	return NULL;
}

void hemem_print_stats()
{
	LOG_STATS(
		"mem_allocated: [%lu]\tpages_allocated: [%lu]\tmissing_faults_handled: [%lu]\tbytes_migrated: [%lu]\tmigrations_up: [%lu]\tmigrations_down: [%lu]\tmigration_waits: [%lu]\n",
		mem_allocated, pages_allocated, missing_faults_handled,
		bytes_migrated, migrations_up, migrations_down,
		migration_waits);
	mmgr_stats();
}

void hemem_clear_stats()
{
	pages_allocated = 0;
	pages_freed = 0;
	missing_faults_handled = 0;
	migrations_up = 0;
	migrations_down = 0;
}

struct hemem_page *get_hemem_page(uint64_t va)
{
	return find_page(va);
}
