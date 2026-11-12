#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t sequence, at, caps;
    size_t bytes;
    const char *function;
    void *task;
} kms_usb_allocation_t;

typedef struct {
    kms_usb_allocation_t failed;
    uint32_t at;
    unsigned address, interface_number;
    size_t free_bytes, largest, dma_free, dma_largest;
    unsigned matched;
} kms_usb_memory_failure_t;

typedef struct {
    kms_usb_allocation_t latest;
    kms_usb_memory_failure_t first, last;
    uint32_t failures;
    size_t free_bytes, largest, dma_free, dma_largest;
} kms_usb_memory_status_t;

void kms_usb_memory_init(void);
// Called outside the allocator callback, while the failed claim's parser and
// sibling interfaces still own their memory. No automatic USB recovery here.
uint32_t kms_usb_memory_sequence(void);
void kms_usb_memory_failed(unsigned address, unsigned interface_number, uint32_t before);
void kms_usb_memory_snapshot(kms_usb_memory_status_t *);
void kms_usb_memory_print(const kms_usb_memory_status_t *);
