#ifndef USERFAULTFD_H
#define USERFAULTFD_H
#include <asm/unistd_64.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <assert.h>

#include "../../include/uapi/linux/userfaultfd.h"
#include "unwrap.h"
#include "log.h"

static inline int uffdio_open(int flags, ulong features)
{
	LOG("%s(flags=0x%x,features=0x%lx)\n", __func__, flags, features);
	int uffd = UNWRAP(syscall(__NR_userfaultfd, flags));

	struct uffdio_api uffdio_api = {
		.api = UFFD_API,
		.features = features,
	};
	UNWRAP(ioctl(uffd, UFFDIO_API, &uffdio_api));
	return uffd;
}

static inline int uffdio_register(int uffd, void *addr, size_t len)
{
	LOG("%s(fd=%d,addr=%p,len=0x%lx)\n", __func__, uffd, addr, len);
	struct uffdio_register args = { .range.start = (__u64)addr,
					.range.len = len,
					.mode = UFFDIO_REGISTER_MODE_MISSING |
						UFFDIO_REGISTER_MODE_WP,
					.ioctls = 0 };
	int ret = UNWRAP(ioctl(uffd, UFFDIO_REGISTER, &args));
	assert(args.ioctls & (1 << _UFFDIO_WRITEPROTECT));
	return ret;
}

static inline int uffdio_unregister(int uffd, void *addr, size_t len)
{
	LOG("%s(fd=%d,addr=%p,len=0x%lx)\n", __func__, uffd, addr, len);
	struct uffdio_range args = {
		.start = (__u64)addr,
		.len = len,
	};
	return ioctl(uffd, UFFDIO_UNREGISTER, &args);
}

static inline int uffdio_writeprotect(int uffd, void *addr, size_t len,
				      ulong mode)
{
	LOG("%s(fd=%d,addr=%p,len=0x%lx,mode=0x%lx)\n", __func__, uffd, addr,
	    len, mode);
	struct uffdio_writeprotect args = {
		.range.start = (__u64)addr,
		.range.len = len,
		.mode = mode,
	};
	return ioctl(uffd, UFFDIO_WRITEPROTECT, &args);
}

static inline int uffdio_wake(int uffd, void *addr, size_t len)
{
	LOG("%s(fd=%d,addr=%p,len=0x%lx)\n", __func__, uffd, addr, len);
	struct uffdio_range args = {
		.start = (__u64)addr,
		.len = len,
	};
	return ioctl(uffd, UFFDIO_WAKE, &args);
}

#endif
