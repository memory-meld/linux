#include <sys/mman.h>
#include <syscall.h>
#include <string.h>
#include <libsyscall_intercept_hook_point.h>

#include "log.h"
#include "param.h"
#include "hemem.h"
#include "interpose.h"

// #include <dlfcn.h>
// static void *bind_symbol(const char *sym)
// {
// 	void *ptr;
// 	if ((ptr = dlsym(RTLD_NEXT, sym)) == NULL) {
// 		fprintf(stderr,
// 			"hemem memory manager interpose: dlsym failed (%s)\n",
// 			sym);
// 		abort();
// 	}
// 	return ptr;
// }

static int mmap_filter(void *addr, size_t len, int prot, int flags, int fd,
		       off_t offset, void **ret)
{
	LOG("%s(addr=%p,len=0x%lx,prot=0x%x,flags=0x%x,fd=%d,offset=0x%lx)\n",
	    __func__, addr, len, prot, flags, fd, offset);

	if (MAP_ANON != (flags & MAP_ANON) ||
	    MAP_STACK == (flags & MAP_STACK) || len < SMALLALLOCSIZE()) {
		LOG("%s: skipped file/stack/small mmap\n", __func__);
		return 1;
	}

	LOG("%s: calling hooked mmap\n", __func__);
	*ret = hemem_mmap(addr, len, prot, flags, fd, offset);
	if (MAP_FAILED == *ret) {
		LOG("%s: hooked mmap failed with errno %d `%s`\n", __func__,
		    errno, strerror(errno));
	}
	return 0;
}

static int munmap_filter(void *addr, size_t len, int *ret)
{
	LOG("%s(addr=%p,len=0x%lx)\n", __func__, addr, len);

	LOG("%s: calling hooked munmap\n", __func__);
	if (-1 == (*ret = hemem_munmap(addr, len))) {
		LOG("%s: hooked munmap failed with errno %d `%s`\n", __func__,
		    errno, strerror(errno));
	}
	return 0;
}

static int hook(long syscall_number, long a0, long a1, long a2, long a3,
		long a4, long a5, long *ret)
{
	// cannot do logging here, otherwise we will end up with a dead loop and a stackoverflow in the end
	switch (syscall_number) {
	case SYS_mmap:
		return mmap_filter((void *)a0, (size_t)a1, (int)a2, (int)a3,
				   (int)a4, (off_t)a5, (void **)ret);
	case SYS_munmap:
		return munmap_filter((void *)a0, (size_t)a1, (int *)ret);
	default:
		// ignored
		return 1;
	}
}

static __attribute__((constructor)) void init(void)
{
	log_init();

	intercept_hook_point = hook;

	LOG_ALWAYS("hemem hook installed\n");
	hemem_init();
}

static __attribute__((destructor)) void hemem_shutdown(void)
{
	hemem_stop();
}
