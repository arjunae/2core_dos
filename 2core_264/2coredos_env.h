#ifndef EDGE264_2CORE_ENV_H
#define EDGE264_2CORE_ENV_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <sys/types.h>

#define _SC_NPROCESSORS_ONLN 1
static inline int sysconf(int name) { return 1; }

#ifndef ENOBUFS
#define ENOBUFS 105
#endif
#ifndef ENOTSUP
#define ENOTSUP 95
#endif
#ifndef EBADMSG
#define EBADMSG 74
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENODATA
#define ENODATA 61
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef ENOMSG
#define ENOMSG 42
#endif

#define CLOCK_PROCESS_CPUTIME_ID 2
struct timespec;
static inline int clock_gettime(int clk_id, struct timespec *tp) { return 0; }

#include <stdlib.h>
#include <string.h>

// We redefine aligned_alloc to support 64-byte alignment using malloc
static inline void* r_aligned_alloc(size_t alignment, size_t size) {
    void* p = malloc(size + alignment + sizeof(void*));
    if (!p) return NULL;
    size_t offset = alignment - ((size_t)p + sizeof(void*)) % alignment;
    void** aligned_p = (void**)((char*)p + sizeof(void*) + offset);
    aligned_p[-1] = p; // store original pointer
    return aligned_p;
}

static inline void r_aligned_free(void* p) {
    if (p) free(((void**)p)[-1]);
}

#define aligned_alloc(a, s) r_aligned_alloc(a, s)
#define free(p) r_aligned_free(p)

typedef int pthread_t;
typedef int pthread_mutex_t;
typedef int pthread_cond_t;
static inline int pthread_mutex_init(pthread_mutex_t *m, void *attr) { return 0; }
static inline int pthread_mutex_destroy(pthread_mutex_t *m) { return 0; }
static inline int pthread_mutex_lock(pthread_mutex_t *m) { return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t *m) { return 0; }
static inline int pthread_cond_init(pthread_cond_t *c, void *attr) { return 0; }
static inline int pthread_cond_destroy(pthread_cond_t *c) { return 0; }
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { return 0; }
static inline int pthread_cond_signal(pthread_cond_t *c) { return 0; }
static inline int pthread_cond_broadcast(pthread_cond_t *c) { return 0; }
static inline int pthread_create(pthread_t *t, void *attr, void *(*func)(void*), void *arg) { return 0; }
static inline int pthread_cancel(pthread_t t) { return 0; }

#endif
