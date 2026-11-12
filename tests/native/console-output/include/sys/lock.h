#pragma once
typedef int _lock_t;
void _lock_acquire(_lock_t *);
void _lock_release(_lock_t *);
