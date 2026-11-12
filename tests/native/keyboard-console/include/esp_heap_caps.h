#pragma once
#include <stddef.h>
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_SPIRAM 2
#define SIZE_MAX ((size_t)-1)
void *heap_caps_malloc(size_t, unsigned);
void heap_caps_free(void *);
