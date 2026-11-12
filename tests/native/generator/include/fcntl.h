#pragma once
#define F_SETFL 0
#define O_NONBLOCK 0
static inline int fcntl(int fd, int cmd, int flags) { (void)fd; (void)cmd; (void)flags; return 0; }
