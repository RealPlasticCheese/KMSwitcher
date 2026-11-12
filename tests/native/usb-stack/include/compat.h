#pragma once
#include <stdint.h>
#include <stddef.h>
int kms_sdk_ffs(int value);
#define __builtin_ffs kms_sdk_ffs
