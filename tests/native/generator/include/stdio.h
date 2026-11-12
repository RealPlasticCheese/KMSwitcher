#pragma once
#include <stddef.h>
static inline int printf(const char *format, ...) { (void)format; return 0; }
static inline int sscanf(const char *s, const char *format, ...) { (void)s; (void)format; return 0; }
static inline int snprintf(char *s, size_t n, const char *format, ...)
{ (void)s; (void)n; (void)format; return 0; }
