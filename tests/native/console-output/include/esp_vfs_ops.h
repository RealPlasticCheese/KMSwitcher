#pragma once
#include <stddef.h>
#include <sys/stat.h>
#include "esp_err.h"
typedef long long ssize_t;
typedef struct {
    ssize_t (*write_p)(void *, int, const void *, size_t);
    int (*open_p)(void *, const char *, int, int);
    int (*close_p)(void *, int);
    int (*fstat_p)(void *, int, struct stat *);
} esp_vfs_fs_ops_t;
esp_err_t esp_vfs_register_fs(const char *, const esp_vfs_fs_ops_t *, int, void *);
esp_err_t esp_vfs_unregister_fs(const char *);
