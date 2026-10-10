#pragma once
#include <stdlib.h>
#include <stddef.h>
#define MALLOC_CAP_DEFAULT   (1 << 12)
#define MALLOC_CAP_INTERNAL  (1 << 11)
#define MALLOC_CAP_DMA       (1 << 3)
#define MALLOC_CAP_8BIT      (1 << 2)
static inline void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return malloc(size); }
static inline void *heap_caps_calloc(size_t n, size_t size, unsigned caps) { (void)caps; return calloc(n, size); }
static inline void heap_caps_free(void *p) { free(p); }
/* Free bytes in the C heap (LVGL, the app); the HAL's heap_caps_* report a different heap. */
size_t watch_heap_free(void);
#define heap_caps_get_free_size(caps) watch_heap_free()
