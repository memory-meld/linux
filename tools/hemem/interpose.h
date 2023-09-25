#ifndef INTERPOSE_H
#define INTERPOSE_H

#include <stdlib.h>
#include <syscall.h>

#include "libsyscall_intercept_hook_point.h"

// function pointers to libc functions
// extern void *(*libc_malloc)(size_t size);
// extern void *(*libc_calloc)(size_t n, size_t size);
// extern void (*libc_free)(void *p);

static inline void *real_mmap(void *addr, size_t len, int prot, int flags,
			      int fd, off_t offset)
{
	return (void *)syscall_no_intercept(SYS_mmap, addr, len, prot, flags,
					    fd, offset);
}
static inline int real_munmap(void *addr, size_t len)
{
	return syscall_no_intercept(SYS_munmap, addr, len);
}

#endif
