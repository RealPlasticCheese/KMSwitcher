#pragma once
#include <stddef.h>
#define MALLOC_CAP_8BIT 1u
#define MALLOC_CAP_SPIRAM 2u
void *heap_caps_malloc(size_t bytes, unsigned caps);
