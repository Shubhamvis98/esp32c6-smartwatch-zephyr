/* One heap for everything, as on ESP-IDF.
 *
 * The Wi-Fi and Bluetooth drivers allocate from Zephyr's kernel heap
 * (k_malloc).  Left alone, the C library would get a second, separate heap
 * from whatever RAM is left, and each would need room for its worst case.
 * Instead malloc() and friends are link-wrapped (see CMakeLists.txt) onto the
 * kernel heap, which is sized to take nearly all free RAM.  k_malloc is
 * thread- and interrupt-safe. */
#include <stddef.h>
#include <string.h>
#include <zephyr/sys/math_extras.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>

/* The C library still owns whatever RAM is left after the kernel heap
 * (z_malloc_heap, ~20 KB).  It is used as an overflow when the kernel heap
 * is full; a pointer's address tells which heap it came from. */
void *__real_malloc(size_t n);
void  __real_free(void *p);
void *__real_aligned_alloc(size_t a, size_t n);
void *__real_realloc(void *p, size_t n);

extern struct k_heap _system_heap;

static bool in_kheap(const void *p)
{
    const struct sys_heap *h = &_system_heap.heap;
    const uint8_t *b = (const uint8_t *)h->init_mem;
    return (const uint8_t *)p >= b && (const uint8_t *)p < b + h->init_bytes;
}

void *__wrap_malloc(size_t n)
{
    void *p = k_malloc(n);
    return p ? p : __real_malloc(n);
}

void __wrap_free(void *p)
{
    if (!p) return;
    if (in_kheap(p)) k_free(p); else __real_free(p);
}

void *__wrap_calloc(size_t n, size_t size)
{
    size_t total;
    if (size_mul_overflow(n, size, &total)) return NULL;
    void *p = __wrap_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *__wrap_aligned_alloc(size_t a, size_t n)
{
    void *p = k_aligned_alloc(a, n);
    return p ? p : __real_aligned_alloc(a, n);
}

void *__wrap_realloc(void *p, size_t n)
{
    if (!p) return __wrap_malloc(n);
    if (n == 0) { __wrap_free(p); return NULL; }
    if (!in_kheap(p)) return __real_realloc(p, n);
    void *q = k_realloc(p, n);
    if (q) return q;
    /* Kernel heap full: move the block to the C library heap */
    q = __real_malloc(n);
    if (!q) return NULL;
    memcpy(q, p, MIN(n, sys_heap_usable_size(&_system_heap.heap, p)));
    k_free(p);
    return q;
}

size_t watch_heap_free(void)
{
    struct sys_memory_stats st;
    if (sys_heap_runtime_stats_get(&_system_heap.heap, &st) != 0) return 0;
    return st.free_bytes;
}
