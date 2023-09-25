#ifndef ENVCONST_H
#define ENVCONST_H

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>

#define GETENV_OR_DEFAULT(type, suffix, fmt)                          \
	static inline type getenv_or_default_##type(char const *name, \
						    type def)         \
	{                                                             \
		char *env = getenv(name);                             \
		if (!env) {                                           \
			return def;                                   \
		}                                                     \
		char *end = NULL;                                     \
		errno = 0;                                            \
		type res = strto##suffix(env, &end, 0);               \
		if (errno) {                                          \
			char buf[512];                                \
			snprintf(buf, 512,                            \
				 "%s(name=%s,default=" fmt            \
				 ") cannot parse `%s`",               \
				 __func__, name, def, env);           \
			perror(buf);                                  \
			exit(-1);                                     \
		}                                                     \
		return res;                                           \
	}

GETENV_OR_DEFAULT(ulong, ul, "%lu");
GETENV_OR_DEFAULT(long, l, "%ld");

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define CONCAT(A, B) A##B

#define ENVCONST(name, type, default)                                       \
	static inline type CONCAT(, name)()                                 \
	{                                                                   \
		static bool initialized = false;                            \
		static type value;                                          \
		if (unlikely(!initialized)) {                               \
			value = getenv_or_default_##type(#name, (default)); \
			initialized = true;                                 \
		}                                                           \
		return value;                                               \
	}

#define ENVSTR(name, default)                      \
	static inline char const *CONCAT(, name)() \
	{                                          \
		static bool initialized = false;   \
		static char const *value;          \
		if (unlikely(!initialized)) {      \
			value = getenv(#name);     \
			if (!value)                \
				value = (default); \
			initialized = true;        \
		}                                  \
		return value;                      \
	}

#endif
