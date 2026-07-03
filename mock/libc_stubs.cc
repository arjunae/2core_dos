#include <stddef.h>
#include <stdint.h>

#define APUTILS_NO_LIBC_ALIASES
#include "include/aputils.h"

extern volatile void* g_tskPtr;
MemBlock* freeList = nullptr;

extern "C" {

void* malloc(size_t size) {
    uint32_t raw = (uint32_t)rmalloc(size + 64);
    if (!raw) return NULL;
    uint32_t aligned = (raw + 4 + 63) & ~63;
    *((uint32_t*)(aligned - 4)) = raw;
    return (void*)aligned;
}

void free(void *ptr) {
    if (!ptr) return;
    uint32_t raw = *((uint32_t*)((uint32_t)ptr - 4));
    rfree((void*)raw);
}

void* calloc(size_t num, size_t size) {
    void* ptr = malloc(num * size);
    if (ptr) {
        uint8_t *d = (uint8_t*)ptr;
        size_t n = num * size;
        while(n--) *d++ = 0;
    }
    return ptr;
}

void* aligned_alloc(size_t alignment, size_t size) {
    return malloc(size);
}

extern "C" {
void *memcpy (void *d, const void *s, size_t n) { return rmemcpy (d, s, (uint32_t)n); }
void *memset (void *d, int c,         size_t n) { return rmemset (d, c, (uint32_t)n); }
void *memmove(void *d, const void *s, size_t n) { return rmemmove(d, s, (uint32_t)n); }
int   memcmp (const void *a, const void *b, size_t n) { return rmemcmp(a, b, (uint32_t)n); }
char  *strcpy(char *d, const char *s)                 { return rstrcpy(d, s); }
int    strcmp(const char *a, const char *b)           { return rstrcmp(a, b); }

}
void __dj_assert(const char* msg, const char* file, int line) {
    if (g_tskPtr) {
        volatile char* dbg = (volatile char*)g_tskPtr + 36;
        const char* s = msg;
        while (*s && (dbg - ((volatile char*)g_tskPtr + 36)) < 510) {
            *dbg++ = *s++;
        }
        *dbg = '\0';
    }
    while(1);
}

int abs(int j) {
    return j < 0 ? -j : j;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    if (size > 0) str[0] = '\0';
    return 0;
}

int __popcountsi2(unsigned int a) {
    int c = 0;
    for (; a; a >>= 1) c += a & 1;
    return c;
}

} // extern "C"
