#ifndef UNWRAP_H
#define UNWRAP_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <execinfo.h>

#define MAX_ERRNO 4095

#define UNWRAP(exp)                                                               \
	({                                                                        \
		typeof(exp) ret = (exp);                                          \
		void *erased = (void *)(long)ret;                                 \
		char *fmt =                                                       \
			__builtin_classify_type(ret) == 5 ?                       \
				"[%s:%d](%s) `%s` Returned `%p` Error %d %s\n" :  \
				"[%s:%d](%s) `%s` Returned `%lld` Error %d %s\n"; \
		if (errno && (erased == NULL || erased == (void *)-1)) {          \
			fprintf(stderr, fmt, __FILE__, __LINE__, __func__,        \
				#exp, erased, errno, strerror(errno));            \
			abort();                                                  \
		}                                                                 \
		ret;                                                              \
	})

#define UNWRAP_ERR(exp)                                                           \
	({                                                                        \
		typeof(exp) ret = (exp);                                          \
		long erased = (long)ret;                                          \
		char *fmt =                                                       \
			__builtin_classify_type(ret) == 5 ?                       \
				"[%s:%d](%s) `%s` Returned `%p` Error %d %s\n" :  \
				"[%s:%d](%s) `%s` Returned `%lld` Error %d %s\n"; \
		if (erased < 0 && erased >= -MAX_ERRNO) {                         \
			int err = (int)-erased;                                   \
			fprintf(stderr, fmt, __FILE__, __LINE__, __func__,        \
				#exp, (void *)erased, err, strerror(err));        \
			abort();                                                  \
		}                                                                 \
		ret;                                                              \
	})

static inline void print_trace(void)
{
	void *array[16];

	int size = backtrace(array, 10);
	char **strings = backtrace_symbols(array, size);
	if (strings != NULL) {
		printf("Obtained %d stack frames.\n", size);
		for (int i = 0; i < size; i++)
			printf("%s\n", strings[i]);
	}

	free(strings);
}

/*
#undef assert
#define assert(x)                      \
	do {                           \
		bool ok = (x);         \
		if (!ok) {             \
			print_trace(); \
			abort();       \
		}                      \
	} while (0)
*/

#endif
