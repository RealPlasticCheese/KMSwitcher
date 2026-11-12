#pragma once
#include <stddef.h>
typedef struct { int descriptor; } FILE;
extern FILE *stdout, *stderr;
#define _IOLBF 1
FILE *freopen(const char *, const char *, FILE *);
int setvbuf(FILE *, char *, int, size_t);
int fflush(FILE *);
