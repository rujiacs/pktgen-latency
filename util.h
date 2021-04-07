#ifndef _PKTGEN_UTIL_H_
#define _PKTGEN_UTIL_H_

#define _GNU_SOURCE

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <limits.h>
#include <sys/syscall.h>
#include <sys/resource.h>

//#define DEBUG_PRINT

#define LOG_ERROR(format, ...) \
		fprintf(stderr, "[ERROR] %s %d: " format "\n", \
						__FILE__, __LINE__, ##__VA_ARGS__);

#define LOG_INFO(format, ...) \
		fprintf(stdout, "[INFO] %s %d: " format "\n", \
						__FILE__, __LINE__, ##__VA_ARGS__);

#ifdef DEBUG_PRINT
#define LOG_DEBUG(format, ...) \
		fprintf(stderr, "[DEBUG] %s %d: " format "\n", \
						__FILE__, __LINE__, ##__VA_ARGS__);
#else
#define LOG_DEBUG(format, ...)
#endif

#define PAGE_SIZE 4096

static inline unsigned int roundup_2(unsigned int num)
{
	num--;

	num |= (num >> 1);
	num |= (num >> 2);
	num |= (num >> 4);
	num |= (num >> 8);
	num |= (num >> 16);
	num++;
	return num;
}

#ifndef offsetof
#define offsetof(type, member) ((size_t) & ((type*)0)->member)
#endif

#ifndef container_of
#define container_of(ptr, type, member) ({ \
			const typeof(((type *)0)->member) *__mptr = (ptr); \
			(type*)((char*)__mptr - offsetof(type,member)); })
#endif

static inline unsigned long long rdtsc(void)
{
	unsigned hi, lo;

	asm volatile("rdtsc":"=a"(lo), "=d"(hi));
	return (((unsigned long long)lo) |
					(((unsigned long long)hi) << 32));
}

static inline bool str_to_int(
				const char *s, int base, int *u)
{
	long val = 0;
	char *tail = NULL;

	val = strtol(s, &tail, base);
	if (errno == EINVAL || errno == ERANGE
					|| tail == s || *tail != '\0') {
		*u = 0;
		return false;
	}

	if (val > INT_MAX) {
		*u = 0;
		errno = ERANGE;
		return false;
	}

	*u = val;
	return true;
}

#endif /* _PKTGEN_UTIL_H_ */
