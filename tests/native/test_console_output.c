// Exercise the production writer against a bounded UART sink and shared streams.
#include "console_output.c"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, mistakes, calls, bytes, locks, unlocks, fail_call;
static unsigned reopen_calls, fail_reopen, buffers, registrations, removals;
static unsigned flushes, max_write;
static esp_err_t register_result;
static const esp_vfs_fs_ops_t *registered;
static char captured[8192];
static FILE out_file, err_file;
FILE *stdout = &out_file, *stderr = &err_file;
int errno;

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
void _lock_acquire(_lock_t *lock)
{ ++locks; if (*lock) ++mistakes; *lock = 1; }
void _lock_release(_lock_t *lock)
{ ++unlocks; if (*lock != 1) ++mistakes; *lock = 0; }
int uart_write_bytes(int uart, const void *data, size_t size)
{
    ++calls;
    if (uart || !output_lock || size > 256 || bytes + size > sizeof(captured)) ++mistakes;
    if (calls == fail_call) return -1;
    if (max_write && size > max_write) size = max_write;
    const char *p = data;
    for (size_t n = 0; n < size; ++n) captured[bytes++] = p[n];
    return (int)size;
}
esp_err_t esp_vfs_register_fs(const char *path, const esp_vfs_fs_ops_t *ops, int flags, void *context)
{
    ++registrations; registered = ops;
    if (!same(path, OUTPUT_PATH) || flags != 3 || context) ++mistakes;
    return register_result;
}
esp_err_t esp_vfs_unregister_fs(const char *path)
{ ++removals; if (!same(path, OUTPUT_PATH)) ++mistakes; return ESP_OK; }
FILE *freopen(const char *path, const char *mode, FILE *file)
{
    ++reopen_calls;
    if (!same(mode, "w") || (file != stdout && file != stderr)) ++mistakes;
    if (!same(path, OUTPUT_PATH) && !same(path, "/dev/console")) ++mistakes;
    if (reopen_calls == fail_reopen) return NULL;
    if (same(path, OUTPUT_PATH) && registered->open_p(NULL, "/", 0, 0) < 0) return NULL;
    return file;
}
int setvbuf(FILE *file, char *buffer, int mode, size_t size)
{
    ++buffers;
    if (mode != _IOLBF) ++mistakes;
    if (file == stdout) {
        if (buffer != stdout_buffer || size != sizeof(stdout_buffer)) ++mistakes;
    } else if (file == stderr) {
        if (buffer != stderr_buffer || size != sizeof(stderr_buffer)) ++mistakes;
    } else ++mistakes;
    return 0;
}
int fflush(FILE *file)
{ if (file != stdout && file != stderr) ++mistakes; ++flushes; return 0; }

__declspec(dllexport) unsigned kms_console_output_test_count(void) { return checks; }
__declspec(dllexport) int kms_run_console_output_tests(void)
{
    // Firmware VFS initialization redirects both shared streams; stdin is untouched.
    CHECK(kms_console_output_init() == ESP_OK);
    CHECK(registrations == 1 && reopen_calls == 2 && buffers == 2 && !removals);
    CHECK(registered->open_p(NULL, "", 0, 0) == 0);
    CHECK(registered->open_p(NULL, "/", 0, 0) == 0);
    CHECK(registered->open_p(NULL, "/bad", 0, 0) == -1 && errno == ENOENT);
    struct stat info;
    CHECK(registered->fstat_p(NULL, 0, &info) == 0 && info.st_mode == S_IFCHR);
    CHECK(registered->close_p(NULL, 0) == 0);
    CHECK(registered->write_p(NULL, 0, "", 0) == 0 && !calls);

    char input[2048];
    for (unsigned n = 0; n < sizeof(input); ++n) input[n] = (char)('a' + n % 26);
    input[254] = '\n'; input[255] = '\n'; input[509] = '\r'; input[510] = '\n';
    input[1023] = 0; input[2047] = '\n';
    CHECK(output_write(NULL, 0, input, sizeof(input)) == sizeof(input));
    CHECK(calls == 9); // Large replies require blocks, rather than thousands of driver calls.
    unsigned index = 0;
    for (unsigned n = 0; n < sizeof(input); ++n) {
        if (input[n] == '\n') { CHECK(captured[index] == '\r'); ++index; }
        CHECK(captured[index] == input[n]); ++index;
    }
    CHECK(bytes == index && !output_lock && locks == unlocks);
    calls = bytes = 0;
    CHECK(output_write(NULL, 0, "x", 1) == 1 && calls == 1 && bytes == 1 && captured[0] == 'x');
    kms_console_output_flush(); CHECK(flushes == 2);
    // A driver short write continues from its actual offset, including CRLF.
    calls = bytes = 0; max_write = 1;
    CHECK(output_write(NULL, 0, "a\nz", 3) == 3 && calls == 4 && bytes == 4);
    CHECK(captured[0] == 'a' && captured[1] == '\r' && captured[2] == '\n' && captured[3] == 'z');
    max_write = 0;

    calls = bytes = 0; fail_call = 1;
    CHECK(output_write(NULL, 0, input, sizeof(input)) == -1 && errno == EIO);
    CHECK(calls == 1 && !bytes && !output_lock);
    calls = bytes = 0; fail_call = 2;
    CHECK(output_write(NULL, 0, input, sizeof(input)) == 255);
    CHECK(calls == 2 && bytes == 256 && !output_lock);
    fail_call = 0;

    reopen_calls = buffers = 0; register_result = ESP_FAIL;
    CHECK(kms_console_output_init() == ESP_FAIL && !reopen_calls && !buffers);
    register_result = ESP_OK; fail_reopen = 2;
    CHECK(kms_console_output_init() == ESP_FAIL);
    CHECK(reopen_calls == 4 && removals == 1 && !buffers);
    CHECK(!mistakes && locks == unlocks);
    return 0;
}
