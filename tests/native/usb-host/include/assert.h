#pragma once
void kms_host_assert(int);
#define assert(value) kms_host_assert(!!(value))
