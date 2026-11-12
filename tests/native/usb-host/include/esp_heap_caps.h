#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "usb/usb_host.h"
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_DMA (1u << 3)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_SPIRAM (1u << 10)
void *heap_caps_calloc(size_t count, size_t bytes, uint32_t caps);
#ifdef KMS_USB_SHARED_HEAP
void *heap_caps_malloc(size_t bytes, uint32_t caps);
void *heap_caps_realloc(void *pointer, size_t bytes, uint32_t caps);
#else
// Ordinary lifecycle fixtures model boards without PSRAM.
static inline void *heap_caps_malloc(size_t bytes, uint32_t caps)
{ (void)bytes; (void)caps; return NULL; }
static inline void *heap_caps_realloc(void *pointer, size_t bytes, uint32_t caps)
{ (void)caps; return realloc(pointer, bytes); }
#endif
typedef void (*esp_alloc_failed_hook_t)(size_t, uint32_t, const char *);
esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t);
size_t heap_caps_get_free_size(uint32_t);
size_t heap_caps_get_largest_free_block(uint32_t);
