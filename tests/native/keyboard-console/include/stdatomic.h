#pragma once
#include <stdbool.h>
// This fixture executes production callbacks on one thread.
typedef bool atomic_bool;
typedef unsigned atomic_uint;
#define atomic_fetch_add(p, value) ((*(p) += (value)) - (value))
#define atomic_load(p) (*(p))
