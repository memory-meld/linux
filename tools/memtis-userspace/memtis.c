// clang -std=gnu2x -shared -o libmemtis.so memtis.c
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAX_ERRNO 4095
#define TRY(exp)                                                               \
	({                                                                     \
		__typeof__((exp)) __err = (exp);                               \
		if ((unsigned long)(__err) >= (unsigned long)(-MAX_ERRNO)) {   \
			fprintf(stderr,                                        \
				"[%s:%d]%s failed with error %ld: %s\n",       \
				__FILE__, __LINE__, __func__, (long)__err,     \
				strerror(-(long)__err));                       \
			return (long)(__err);                                  \
		}                                                              \
		__err;                                                         \
	})

#define UNWRAP(exp)                                                            \
	({                                                                     \
		__typeof__((exp)) __err = (exp);                               \
		if ((unsigned long)(__err) >= (unsigned long)(-MAX_ERRNO)) {   \
			fprintf(stderr,                                        \
				"[%s:%d]%s failed with error %ld: %s\n",       \
				__FILE__, __LINE__, __func__, (long)__err,     \
				strerror(-(long)__err));                       \
			exit((long)(__err));                                   \
		}                                                              \
		__err;                                                         \
	})

#define CGROUP_PATH "/sys/fs/cgroup"
#define CGROUP_NAME "memtis"

static int syscall_htmm_start = 449;
static int syscall_htmm_end = 450;

inline static long htmm_start(pid_t pid, int node)
{
	return syscall(syscall_htmm_start, pid, node);
}

inline static long htmm_end(pid_t pid)
{
	return syscall(syscall_htmm_end, pid);
}

inline static ssize_t echo(char const *name, char *buf, size_t len)
{
	fprintf(stderr, "echo \"%s\" > \"%s\"\n", buf, name);
	int fd = TRY(open(name, O_WRONLY));
	ssize_t written = TRY(write(fd, buf, len));
	TRY(close(fd));
	return written;
}

inline static int htmm_cgroup_create(void)
{
	struct stat st;
	TRY(stat(CGROUP_PATH "/" CGROUP_NAME, &st));
	if (st.st_mode & S_IFDIR) {
		return 0;
	}
	TRY(mkdir(CGROUP_PATH "/" CGROUP_NAME, 0755));
	return 0;
}

inline static int htmm_cgroup_enable(pid_t pid)
{
	char buf[64];
	TRY(echo(CGROUP_PATH "/cgroup.subtree_control", "+memory +cpuset", 15));
	TRY(echo(CGROUP_PATH "/" CGROUP_NAME "/cgroup.procs", buf,
		 TRY(snprintf(buf, sizeof(buf), "%d", pid))));
	TRY(echo(CGROUP_PATH "/" CGROUP_NAME "/memory.htmm_enabled", "enabled",
		 7));
	char *dram_node = getenv("DRAM_NODE");
	char *dram_limit = getenv("DRAM_LIMIT");
	if (dram_node && dram_limit) {
		fprintf(stderr, "setting dram limit: node=%s limit=%s\n",
			dram_node, dram_limit);
		char path[256];
		TRY(snprintf(path, sizeof(path),
			     CGROUP_PATH "/" CGROUP_NAME
					 "/memory.max_at_node%s",
			     dram_node));
		TRY(echo(path, dram_limit, strlen(dram_limit)));
	}

	return 0;
}

static pid_t pid = 0;
static __attribute__((constructor)) void ctor(void)
{
	fprintf(stderr, "preload library started\n");
	pid = UNWRAP(getpid());
	fprintf(stderr, "starting htmm for pid=%d\n", pid);
	UNWRAP(htmm_cgroup_create());
	UNWRAP(htmm_cgroup_enable(pid));
	// the node argument is unused for now
	UNWRAP(htmm_start(UNWRAP(getpid()), 0));
}

static __attribute__((destructor)) void dtor(void)
{
	pid_t newpid = UNWRAP(getpid());
	if (pid != newpid) {
		fprintf(stderr, "pid mismatch? newpid=%d\n", newpid);
	}
	UNWRAP(htmm_end(pid));
	fprintf(stderr, "preload library stopped\n");
}
