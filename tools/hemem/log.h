#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <stdarg.h>

extern FILE *hememlogf;
extern FILE *statsf;
extern FILE *timef;
// extern bool timing;
// static inline void log_time(FILE *f, const char *fmt, ...)
// {
// 	if (!timing)
// 		return;
// 	va_list args;
// 	va_start(args, fmt);
// 	vfprintf(f, fmt, args);
// 	va_end(args);
// }

#define LOG_ALWAYS(...) fprintf(stderr, __VA_ARGS__)
#ifdef NLOG
#define LOG(...)
#define LOG_TIME(...)
#define LOG_STATS(...)
#else
// #define LOG(...)
#define LOG(...) fprintf(hememlogf, __VA_ARGS__)
// #define LOG(...) fprintf(stderr, __VA_ARGS__)
// #define LOG_TIME(...) log_time(timef, __VA_ARGS__)
#define LOG_TIME(...) fprintf(timef, __VA_ARGS__)
#define LOG_STATS(...) fprintf(statsf, __VA_ARGS__)
#endif

// #define LOG(...) fprintf(stderr, __VA_ARGS__)
// #define LOG_TIME(str, ...) fprintf(stderr, str,  __VA_ARGS__)
// #define LOG_TIME(str, ...) fprintf(timef, str, __VA_ARGS__)
// #define LOG_STATS(str, ...) fprintf(stderr, str, __VA_ARGS__)

#endif
