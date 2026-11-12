#include "diagnostic_log.h"
#include "kms_log_buffer.h"
#include "kms_runtime.h"
#include "kms_memory.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { LOG_RECORDS = 8192, LIVE_LOG_RECORDS = 32, TYPE_TIMEOUT_MS = 3000, TYPE_STEP_MS = 5 };
static kms_log_buffer_t buffer;
static SemaphoreHandle_t mutex;
static QueueHandle_t requests;
typedef struct { uint16_t length; char text[KMS_LOG_TEXT_BYTES]; } live_log_t;
static QueueHandle_t live_output;
static atomic_uint live_dropped;
static vprintf_like_t previous_print = vprintf;
static atomic_bool interactive;
static bool command_ready, command_overflow;
static bool output_paused;
static char command[KMS_COMMAND_BYTES];
static unsigned command_length;

static void append_output(const char *text, size_t length)
{
    if (!mutex || !buffer.records || !length) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    kms_log_buffer_append(&buffer, kms_now(), text, length);
    xSemaphoreGive(mutex);
}

// Capture plain command responses as well as ESP logs, including worker output.
int __wrap_printf(const char *format, ...)
{
    char text[KMS_LOG_TEXT_BYTES + 1];
    va_list args, copy;
    va_start(args, format);
    if (!interactive) { int result = vprintf(format, args); va_end(args); return result; }
    va_copy(copy, args);
    int length = vsnprintf(text, sizeof(text), format, copy);
    va_end(copy);
    if (interactive && length > 0) append_output(text, (size_t)length);
    int result = vprintf(format, args); va_end(args); return result;
}
int __real_puts(const char *text);
int __wrap_puts(const char *text)
{
    if (interactive) { append_output(text, strlen(text)); append_output("\n", 1); }
    return __real_puts(text);
}
int __real_putchar(int c);
int __wrap_putchar(int c)
{
    char text = (char)c;
    if (interactive) append_output(&text, 1);
    return __real_putchar(c);
}
int __real_fputs(const char *text, FILE *stream);
int __wrap_fputs(const char *text, FILE *stream)
{
    if (interactive && stream == stdout) append_output(text, strlen(text));
    return __real_fputs(text, stream);
}
size_t __real_fwrite(const void *data, size_t size, size_t count, FILE *stream);
size_t __wrap_fwrite(const void *data, size_t size, size_t count, FILE *stream)
{
    if (interactive && stream == stdout && size && count <= SIZE_MAX / size) append_output(data, size * count);
    return __real_fwrite(data, size, count, stream);
}

static struct {
    uint64_t cursor, end;
    kms_log_record_t record;
    unsigned offset, slot;
    uint32_t progress, sent;
    bool release;
} typing;

static int capture(const char *format, va_list args)
{
    char text[KMS_LOG_TEXT_BYTES + 1];
    va_list copy; va_copy(copy, args);
    int length = vsnprintf(text, sizeof(text), format, copy); va_end(copy);
    if (length > 0) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        kms_log_buffer_append(&buffer, kms_now(), text, (size_t)length);
        xSemaphoreGive(mutex);
    }
    // UART transmission can take tens of milliseconds at 115200. Never make
    // input/ACK processing wait for it, even during a fault storm or full queue.
    if (!live_output) return previous_print(format, args);
    if (length > 0) {
        live_log_t record;
        record.length = (uint16_t)((size_t)length > sizeof(record.text) ? sizeof(record.text) : (size_t)length);
        memcpy(record.text, text, record.length);
        if ((size_t)length > sizeof(record.text)) record.text[record.length - 1] = '\n';
        if (xQueueSend(live_output, &record, 0) != pdTRUE) atomic_fetch_add(&live_dropped, 1);
    }
    return length;
}
static void write_live(const char *format, ...)
{
    va_list args; va_start(args, format);
    (void)previous_print(format, args); va_end(args);
}
static bool drain_live(void)
{
    live_log_t record;
    if (xQueueReceive(live_output, &record, portMAX_DELAY) != pdTRUE) return false;
    write_live("%.*s", (int)record.length, record.text);
    return true;
}
static void live_worker(void *arg)
{ (void)arg; while (true) (void)drain_live(); }
static bool snapshot(uint64_t *cursor, uint64_t *end)
{
    if (!buffer.records) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    kms_log_buffer_expire(&buffer, kms_now());
    *cursor = buffer.first; *end = buffer.end;
    xSemaphoreGive(mutex); return true;
}
static bool read_record(uint64_t cursor, kms_log_record_t *record)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool valid = kms_log_buffer_read(&buffer, cursor, record);
    xSemaphoreGive(mutex); return valid;
}
static void print_status(void)
{
    if (!buffer.records) { printf("KMSLOG unavailable: PSRAM allocation or worker initialization failed\n"); return; }
    xSemaphoreTake(mutex, portMAX_DELAY);
    kms_log_buffer_expire(&buffer, kms_now());
    uint64_t count = buffer.end - buffer.first, evicted = buffer.evicted, truncated = buffer.truncated;
    uint32_t oldest = count ? kms_now() - buffer.records[buffer.first % buffer.capacity].at : 0;
    xSemaphoreGive(mutex);
    printf("KMSLOG records=%" PRIu64 " capacity=%u bytes=%u oldest_ms=%" PRIu32
           " retention_ms=%u evicted=%" PRIu64 " truncated=%" PRIu64
           " live_async=%s live_dropped=%u volatile=yes\n",
           count, LOG_RECORDS, (unsigned)(LOG_RECORDS * sizeof(kms_log_record_t)), oldest,
           KMS_LOG_KEEP_MS, evicted, truncated, live_output ? "yes" : "no", atomic_load(&live_dropped));
}
static void worker(void *arg)
{
    (void)arg; bool dump;
    while (true) {
        if (xQueueReceive(requests, &dump, portMAX_DELAY) != pdTRUE) continue;
        if (!dump) { print_status(); continue; }
        uint64_t cursor, end;
        if (!snapshot(&cursor, &end)) { print_status(); continue; }
        print_status(); printf("KMSLOG BEGIN\n");
        kms_log_record_t record;
        while (cursor < end) {
            if (!read_record(cursor++, &record)) { printf("\nKMSLOG overwritten during export\n"); break; }
            fwrite(record.text, 1, record.length, stdout);
            vTaskDelay(1);
        }
        printf("\nKMSLOG END\n");
    }
}
void kms_diagnostic_log_init(void)
{
    buffer.capacity = LOG_RECORDS;
    buffer.records = heap_caps_malloc(LOG_RECORDS * sizeof(kms_log_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mutex = xSemaphoreCreateMutex(); requests = xQueueCreate(2, sizeof(bool));
    if (!buffer.records || !mutex || !requests ||
        xTaskCreate(worker, "kms-log", 4096, NULL, 1, NULL) != pdPASS) {
        heap_caps_free(buffer.records); buffer.records = NULL;
        if (mutex) vSemaphoreDelete(mutex);
        if (requests) vQueueDelete(requests);
        requests = NULL;
        ESP_LOGW("diagnostics", "PSRAM log buffer unavailable; normal input continues"); return;
    }
    live_output = kms_memory_queue_create(LIVE_LOG_RECORDS, sizeof(live_log_t));
    if (live_output && xTaskCreate(live_worker, "kms-log-uart", 3072, NULL, 1, NULL) != pdPASS) {
        kms_memory_queue_delete(live_output); live_output = NULL;
    }
    previous_print = esp_log_set_vprintf(capture);
    ESP_LOGI("diagnostics", "PSRAM log buffer ready: %u bytes, up to one hour", (unsigned)(LOG_RECORDS * sizeof(kms_log_record_t)));
}
bool kms_diagnostic_log_console(const char *line)
{
    bool dump = !strcmp(line, "logs");
    if (!dump && strcmp(line, "logs-status")) return false;
    if (!requests) print_status();
    else if (xQueueSend(requests, &dump, 0) != pdTRUE) printf("KMSLOG busy; retry after export\n");
    return true;
}
bool kms_diagnostic_log_type(void *ctx)
{
    (void)ctx;
    if (!kms_link_typing_ready() || !snapshot(&typing.cursor, &typing.end) || typing.cursor == typing.end) return false;
    interactive = command_ready = command_overflow = false; command_length = 0;
    output_paused = false;
    typing.record.length = 0; typing.offset = 0; typing.release = false;
    typing.slot = kms_link_selected(); typing.progress = kms_now(); typing.sent = kms_now();
    kms_link_text_begin();
    return true;
}
void kms_diagnostic_log_stop(void *ctx)
{
    (void)ctx;
    interactive = command_ready = command_overflow = false; command_length = 0;
    output_paused = false;
    // Clear queued diagnostic reports and release any synthetic held key.
    kms_link_clear_input();
}
bool kms_diagnostic_console_input(void *ctx, const kms_state_t *previous, const kms_state_t *state)
{
    (void)ctx;
    if (!interactive) {
        // I is HID usage 12; entering consumes the key instead of cancelling.
        if (!kms_key(state, 12) || kms_key(previous, 12) || state->modifiers) return false;
        // Preserve the active route and its acknowledgement state. Clearing
        // input raises a link fault, which would cancel this session next tick.
        // Cancel receiver text without resetting the route, then release any
        // pending key from the compatibility path with a neutral report.
        kms_link_text_cancel();
        kms_state_t released = {0};
        kms_link_emit(NULL, &released, 0, 0, 0, 0);
        uint64_t end;
        if (!snapshot(&typing.cursor, &end)) return false;
        typing.cursor = end; typing.offset = typing.record.length = 0; typing.release = false;
        interactive = true; command_length = 0; command_ready = command_overflow = false;
        typing.progress = kms_now();
        static const char welcome[] = "\nKeyboard Console\nType help then Enter for commands. Escape stops output; Escape when idle exits.\n";
        append_output(welcome, sizeof(welcome) - 1);
        return true;
    }
    if (kms_key(state, 41) && !kms_key(previous, 41)) {
        uint64_t first, end;
        if (!snapshot(&first, &end)) return false;
        bool busy = !output_paused && (typing.cursor < end || typing.offset < typing.record.length ||
                                      typing.release || !kms_link_typing_ready());
        if (!busy) return false;
        // Discard the current response and suppress late worker output until
        // the user starts another command. Preserve the route and release keys.
        typing.cursor = typing.end = end;
        typing.offset = typing.record.length = 0; typing.release = false;
        typing.progress = kms_now(); output_paused = true;
        command_length = 0; command_ready = command_overflow = false;
        kms_link_text_cancel();
        kms_state_t released = {0}; kms_link_emit(NULL, &released, 0, 0, 0, 0);
        return true;
    }
    if (command_ready) return true;
    uint8_t leds;
    if (!kms_link_keyboard_leds(&leds)) return true;
    for (unsigned key = 4; key <= 56; ++key) {
        if (!kms_key(state, key) || kms_key(previous, key)) continue;
        if (output_paused && (key == 40 || key == 42 || key == 44 || (key >= 4 && key <= 39) || key >= 45)) {
            uint64_t first, end;
            if (snapshot(&first, &end)) typing.cursor = typing.end = end;
            output_paused = false;
        }
        if (key == 40) {
            append_output("\n", 1);
            if (command_overflow) {
                append_output("Command too long; discarded.\n", sizeof("Command too long; discarded.\n") - 1);
                command_length = 0; command_overflow = false;
            } else { command[command_length] = 0; command_ready = true; }
            break;
        }
        if (key == 42) {
            if (command_length && !command_overflow) { --command_length; append_output("\b", 1); }
            continue;
        }
        // Decode the same US HID layout used for synthetic output. Ignore host shortcuts.
        if (state->modifiers & (uint8_t)~0x22) continue;
        for (unsigned c = 32; c < 127; ++c) {
            uint8_t usage, modifiers;
            kms_log_key((char)c, (leds & 2) != 0, &usage, &modifiers);
            if (usage != key || !!modifiers != !!(state->modifiers & 0x22)) continue;
            if (command_length + 1 < sizeof(command) && !command_overflow) {
                command[command_length++] = (char)c;
                char echo = (char)c; append_output(&echo, 1);
            } else command_overflow = true;
            break;
        }
    }
    return true;
}
bool kms_diagnostic_console_command(char *line, unsigned capacity)
{
    if (!interactive || !command_ready || command_length + 1 > capacity) return false;
    memcpy(line, command, command_length + 1);
    command_length = 0; command_ready = false; return true;
}
static void finish(kms_router_t *router, const char *reason)
{
    kms_diagnostic_log_stop(NULL); router->diagnostic_typing = false;
    kms_router_block_held(router);
    ESP_LOGI("diagnostics", "Log typing %s", reason);
}
// Return available text immediately. A short record or an idle interactive
// console must never wait for a full radio batch or a terminating newline.
static bool next_character(char *out)
{
    while (true) {
        if (typing.offset == typing.record.length) {
            if (typing.cursor == typing.end) return false;
            if (!read_record(typing.cursor, &typing.record)) return false;
            ++typing.cursor; typing.offset = 0;
        }
        char c = typing.record.text[typing.offset++];
        if (interactive && c == '\x1b') {
            if (typing.offset < typing.record.length && typing.record.text[typing.offset] == '[') {
                ++typing.offset;
                while (typing.offset < typing.record.length) {
                    char next = typing.record.text[typing.offset++];
                    if (next >= '@' && next <= '~') break;
                }
            }
            continue;
        }
        if (c == '\r') continue;
        *out = c; return true;
    }
}
void kms_diagnostic_log_tick(kms_router_t *router)
{
    // Prune even on a quiet system, so clock wrap cannot resurrect old messages.
    if (buffer.records) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        kms_log_buffer_expire(&buffer, kms_now());
        xSemaphoreGive(mutex);
    }
    if (!router->diagnostic_typing) return;
    uint32_t now = kms_now();
    if (interactive) {
        uint64_t end;
        if (snapshot(&end, &typing.end) && output_paused) typing.cursor = typing.end;
        if (typing.cursor == typing.end &&
            typing.offset == typing.record.length && !typing.release && kms_link_typing_ready()) typing.progress = now;
    }
    if (kms_link_selected() != typing.slot || !kms_link_typing_available() ||
        kms_time_elapsed(now, typing.progress, TYPE_TIMEOUT_MS)) {
        finish(router, "stopped: receiver changed or stalled"); return;
    }
    if (!kms_link_typing_ready() || !kms_time_elapsed(now, typing.sent, TYPE_STEP_MS)) return;
    // Wait for the shortcut keys and any existing mouse buttons to be released.
    kms_state_t zero = {0};
    for (unsigned n = 0; n < KMS_INPUTS; ++n)
        if (!kms_state_equal(&router->inputs[n].physical, &zero)) return;
    if (kms_link_text_supported()) {
        uint8_t leds;
        if (!kms_link_keyboard_leds(&leds)) { finish(router, "stopped: keyboard LED state unknown"); return; }
        char text[KMS_TEXT_BYTES]; size_t length = 0;
        while (length < sizeof(text) && next_character(&text[length])) ++length;
        if (length) {
            if (!kms_link_text_send(text, length)) { finish(router, "stopped: text delivery unavailable"); return; }
            typing.progress = typing.sent = now;
        } else if (typing.cursor < typing.end) finish(router, "stopped: buffer overwritten");
        else if (!interactive) finish(router, "completed");
        return;
    }
    kms_state_t state = {0};
    if (typing.release) typing.release = false;
    else {
        uint8_t usage, modifiers, leds;
        // Caps Lock feedback must be known to preserve the case of the log text.
        if (!kms_link_keyboard_leds(&leds)) { finish(router, "stopped: keyboard LED state unknown"); return; }
        char c;
        if (!next_character(&c)) {
            if (typing.cursor < typing.end) finish(router, "stopped: buffer overwritten");
            else if (!interactive) finish(router, "completed");
            return;
        }
        kms_log_key(c, (leds & KMS_LOCK_CAPS) != 0, &usage, &modifiers);
        kms_set_key(&state, usage, true); state.modifiers = modifiers; typing.release = true;
    }
    kms_link_emit(NULL, &state, 0, 0, 0, 0);
    typing.progress = typing.sent = now;
}
