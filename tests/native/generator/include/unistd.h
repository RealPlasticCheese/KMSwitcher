#pragma once
#define STDIN_FILENO 0
static inline int read(int fd, void *data, unsigned n) { (void)fd; (void)data; (void)n; return 0; }
