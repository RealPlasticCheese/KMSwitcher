#pragma once
#include <stddef.h>
#include <stdarg.h>
typedef struct { int unused; } FILE;
extern FILE *stdout;
int printf(const char *, ...);
int vprintf(const char *, va_list);
int vsnprintf(char *, size_t, const char *, va_list);
size_t fwrite(const void *, size_t, size_t, FILE *);
