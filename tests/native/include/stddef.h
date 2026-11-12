#pragma once
typedef unsigned long long size_t;
typedef long long ptrdiff_t;
#define offsetof(type, member) ((size_t)&(((type *)0)->member))
#define NULL ((void *)0)
