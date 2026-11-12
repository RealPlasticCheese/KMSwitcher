#include "console_output.h"
#include <errno.h>
#include <stdio.h>
#include <sys/lock.h>
#include <sys/stat.h>
#include "driver/uart.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"

#define OUTPUT_PATH "/dev/kms-output"
static _lock_t output_lock;
static char stdout_buffer[1024], stderr_buffer[256];

static ssize_t output_write(void *context, int fd, const void *data, size_t size)
{
    (void)context; (void)fd;
    const char *input = data;
    size_t consumed = 0;
    _lock_acquire(&output_lock);
    while (consumed < size) {
        char block[256];
        size_t length = 0, next = consumed;
        // Preserve the console's LF -> CRLF conversion, including at block edges.
        while (next < size && length + 2 <= sizeof(block)) {
            char ch = input[next++];
            if (ch == '\n') block[length++] = '\r';
            block[length++] = ch;
        }
        // The SDK UART VFS calls uart_write_bytes once per character. Sending a
        // block avoids a driver mutex/critical-section round trip for every byte.
        size_t written = 0;
        while (written < length) {
            int result = uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, block + written, length - written);
            if (result <= 0) {
                _lock_release(&output_lock);
                errno = EIO;
                return consumed ? (ssize_t)consumed : -1;
            }
            written += (size_t)result;
        }
        consumed = next;
    }
    _lock_release(&output_lock);
    return (ssize_t)consumed;
}

static int output_open(void *context, const char *path, int flags, int mode)
{
    (void)context; (void)flags; (void)mode;
    // An exact VFS mount match is presented as "/", rather than an empty path.
    if (*path && (path[0] != '/' || path[1])) { errno = ENOENT; return -1; }
    return 0;
}

static int output_close(void *context, int fd)
{
    (void)context; (void)fd;
    return 0;
}

static int output_stat(void *context, int fd, struct stat *result)
{
    (void)context; (void)fd;
    *result = (struct stat){.st_mode = S_IFCHR};
    return 0;
}

esp_err_t kms_console_output_init(void)
{
    static const esp_vfs_fs_ops_t operations = {
        .write_p = output_write, .open_p = output_open,
        .close_p = output_close, .fstat_p = output_stat,
    };
    esp_err_t error = esp_vfs_register_fs(OUTPUT_PATH, &operations,
                                        ESP_VFS_FLAG_STATIC | ESP_VFS_FLAG_CONTEXT_PTR, NULL);
    if (error != ESP_OK) return error;
    // Reopen shared FILE objects during startup, before application workers,
    // so ESP_LOG output and plain printf/protocol replies use block writes.
    if (!freopen(OUTPUT_PATH, "w", stdout) || !freopen(OUTPUT_PATH, "w", stderr)) {
        freopen("/dev/console", "w", stdout);
        freopen("/dev/console", "w", stderr);
        esp_vfs_unregister_fs(OUTPUT_PATH);
        return ESP_FAIL;
    }
    setvbuf(stdout, stdout_buffer, _IOLBF, sizeof(stdout_buffer));
    setvbuf(stderr, stderr_buffer, _IOLBF, sizeof(stderr_buffer));
    return ESP_OK;
}
void kms_console_output_flush(void)
{
    fflush(stdout);
    fflush(stderr);
}
