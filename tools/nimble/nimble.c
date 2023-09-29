#include <limits.h>
#include <sys/wait.h>

#include "nimble.h"

void usage(char *arg0)
{
	fprintf(stderr, "Usage: %s COMMAND [ARGS]...\n", arg0);
}

int main(int argc, char *argv[])
{
	errno = 0;
	if (argc < 2) {
		usage(argv[0]);
		exit(-EINVAL);
	}

	int child = UNWRAP(fork());
	if (child == 0) {
		// child
		UNWRAP(execvp(argv[1], argv + 1));
		return 0;
	}

	printf("waiting for child %d\n", child);
	int status = 0, dead = 0;
	while (!(dead = UNWRAP(waitpid(child, &status, WNOHANG)))) {
		mm_manage(child, ULONG_MAX, PMEM_NODE, DRAM_NODE,
			  MPOL_NF_SHRINK_LISTS);
		sleep_ms(1000);
	}
	// UNWRAP(wait(&status));
	printf("child %d exited\n", dead);
	return 0;
}
