#define DEFINE_REAL_FUNCTIONS
#include "fitcache_internal.h"
#include <stdio.h>

REAL_DEF(read, ssize_t, (int, void*, size_t));
REAL_DEF(write, ssize_t, (int, const void*, size_t));
REAL_DEF(open, int, (const char*, int, ...));
REAL_DEF(open64, int, (const char*, int, ...));
REAL_DEF(close, int, (int));
REAL_DEF(pread, ssize_t, (int, void*, size_t, off_t));
REAL_DEF(readv, ssize_t, (int, const struct iovec*, int));
REAL_DEF(lseek, off_t, (int, off_t, int));
REAL_DEF(lseek64, off64_t, (int, off64_t, int));
REAL_DEF(read64, ssize_t, (int, void*, size_t));
REAL_DEF(fopen, FILE*, (const char*, const char*));
REAL_DEF(fopen64, FILE*, (const char*, const char*));