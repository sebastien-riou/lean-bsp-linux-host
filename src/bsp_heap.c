/* Heap high-water mark: the application and the BSP are linked with --wrap=malloc,free,calloc,realloc,
 * so the allocations made by their own code pass through here. The C library's internal allocations
 * (stdio buffers and the like) are not counted. */
#define _GNU_SOURCE
#include <malloc.h>
#include "bsp_api.h"

void *__real_malloc(size_t size);
void __real_free(void *ptr);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *ptr, size_t size);

static size_t in_use, peak, base;

static void grow(size_t n) {
    size_t now = __atomic_add_fetch(&in_use, n, __ATOMIC_RELAXED);
    size_t seen = __atomic_load_n(&peak, __ATOMIC_RELAXED);
    while (now > seen && !__atomic_compare_exchange_n(&peak, &seen, now, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
}

static void shrink(size_t n) { __atomic_sub_fetch(&in_use, n, __ATOMIC_RELAXED); }

void *__wrap_malloc(size_t size) {
    void *p = __real_malloc(size);
    if (p) grow(malloc_usable_size(p));
    return p;
}

void __wrap_free(void *ptr) {
    if (ptr) shrink(malloc_usable_size(ptr));
    __real_free(ptr);
}

void *__wrap_calloc(size_t n, size_t size) {
    void *p = __real_calloc(n, size);
    if (p) grow(malloc_usable_size(p));
    return p;
}

void *__wrap_realloc(void *ptr, size_t size) {
    size_t old = ptr ? malloc_usable_size(ptr) : 0;
    void *p = __real_realloc(ptr, size);
    if (p) {
        shrink(old);
        grow(malloc_usable_size(p));
    } else if (ptr && size == 0) {
        shrink(old);  /* glibc frees the block on realloc(ptr, 0) */
    }
    return p;
}

void bsp_heap_usage_reset(void) {
    size_t now = __atomic_load_n(&in_use, __ATOMIC_RELAXED);
    __atomic_store_n(&base, now, __ATOMIC_RELAXED);
    __atomic_store_n(&peak, now, __ATOMIC_RELAXED);
}

size_t bsp_heap_usage(void) {
    size_t p = __atomic_load_n(&peak, __ATOMIC_RELAXED);
    size_t b = __atomic_load_n(&base, __ATOMIC_RELAXED);
    return p > b ? p - b : 0;
}
