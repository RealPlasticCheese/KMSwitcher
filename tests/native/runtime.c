#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n)
{ unsigned char *a = d; const unsigned char *b = s; for (size_t i = 0; i < n; ++i) a[i] = b[i]; return d; }
void *memset(void *d, int v, size_t n)
{ unsigned char *a = d; for (size_t i = 0; i < n; ++i) a[i] = (unsigned char)v; return d; }
int memcmp(const void *a, const void *b, size_t n)
{ const unsigned char *x = a, *y = b; for (size_t i = 0; i < n; ++i) if (x[i] != y[i]) return (int)x[i] - y[i]; return 0; }
