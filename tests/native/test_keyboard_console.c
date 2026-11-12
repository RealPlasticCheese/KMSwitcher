// Run the actual console transition and typing pump with a fault-raising link adapter.
#include "../../firmware/controller/main/diagnostic_log.c"
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, clears, emitted, selected = 2;
static uint32_t clock_ms;
static unsigned uart_calls, uart_delay, queue_waits, live_count, live_head;
static live_log_t live_records[2];
static bool fault, available = true;
static bool batched, batch_pending;
static unsigned batches, last_batch, text_cancels;
static kms_log_record_t records[128];
static FILE output;
FILE *stdout = &output;
size_t strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a - *b; }
int vprintf(const char *f, va_list a) { (void)a; ++uart_calls; clock_ms += uart_delay; return (int)strlen(f); }
int vsnprintf(char *out, size_t size, const char *f, va_list a)
{ (void)a; size_t n = strlen(f), copy = n < size - 1 ? n : size - 1; memcpy(out, f, copy); out[copy] = 0; return (int)n; }
int printf(const char *f, ...) { return (int)strlen(f); }
size_t fwrite(const void *p, size_t s, size_t n, FILE *f) { (void)p; (void)s; (void)f; return n; }
int __real_puts(const char *s) { return (int)strlen(s); }
int __real_putchar(int c) { return c; }
int __real_fputs(const char *s, FILE *f) { (void)f; return (int)strlen(s); }
size_t __real_fwrite(const void *p, size_t s, size_t n, FILE *f) { return fwrite(p,s,n,f); }
void kms_console_fake_log(const char *t, const char *f, ...) { (void)t; (void)f; }
vprintf_like_t esp_log_set_vprintf(vprintf_like_t f) { (void)f; return vprintf; }
void *heap_caps_malloc(size_t n, unsigned c) { (void)n; (void)c; return NULL; }
void heap_caps_free(void *p) { (void)p; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xSemaphoreTake(SemaphoreHandle_t s, uint32_t t) { (void)s; (void)t; return 1; }
int xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return 1; }
void vSemaphoreDelete(SemaphoreHandle_t s) { (void)s; }
QueueHandle_t xQueueCreate(unsigned n, unsigned s) { (void)n; (void)s; return NULL; }
int xQueueSend(QueueHandle_t q, const void *p, uint32_t t)
{
    if (q != (void *)3) return 0;
    if (t) ++queue_waits;
    if (live_count == 2) return 0;
    live_records[(live_head + live_count++) % 2] = *(const live_log_t *)p;
    return 1;
}
int xQueueReceive(QueueHandle_t q, void *p, uint32_t t)
{
    (void)t;
    if (q != (void *)3 || !live_count) return 0;
    *(live_log_t *)p = live_records[live_head++ % 2]; --live_count; return 1;
}
void vQueueDelete(QueueHandle_t q) { (void)q; }
QueueHandle_t kms_memory_queue_create(unsigned count, unsigned bytes) { return xQueueCreate(count, bytes); }
void kms_memory_queue_delete(QueueHandle_t q) { vQueueDelete(q); }
int xTaskCreate(void (*f)(void *), const char *n, unsigned s, void *a, unsigned p, void *h)
{ (void)f; (void)n; (void)s; (void)a; (void)p; (void)h; return 0; }
void vTaskDelay(unsigned t) { (void)t; }
uint32_t kms_now(void) { return clock_ms; }
unsigned kms_link_selected(void) { return selected; }
bool kms_link_typing_available(void) { return available; }
bool kms_link_typing_ready(void) { return available && !batch_pending; }
void kms_link_text_begin(void) { }
bool kms_link_text_supported(void) { return batched; }
bool kms_link_text_send(const char *text, size_t length)
{ (void)text; ++batches; last_batch = (unsigned)length; emitted += (unsigned)length; batch_pending = true; return true; }
void kms_link_text_cancel(void) { ++text_cancels; batch_pending = false; }
bool kms_link_keyboard_leds(uint8_t *leds) { *leds = 0; return true; }
void kms_link_clear_input(void) { ++clears; fault = true; batch_pending = false; }
void kms_link_emit(void *ctx, const kms_state_t *s, int32_t x, int32_t y, int32_t w, int32_t p)
{ (void)ctx; (void)x; (void)y; (void)w; (void)p; for (unsigned n = 0; n < KMS_KEY_BYTES; ++n) if (s->keys[n]) ++emitted; }
static void tick(kms_router_t *r)
{
    clock_ms += 10;
    // Match the application loop: a link-clear fault cancels synthetic typing.
    if (fault) { if (r->diagnostic_typing) kms_diagnostic_log_stop(NULL); r->diagnostic_typing = false; fault = false; }
    kms_diagnostic_log_tick(r);
}
static void key(kms_router_t *r, unsigned usage)
{
    kms_sample_t s = {0}; if (usage) kms_set_key(&s.state, usage, true);
    kms_router_input(r, 0, &s, clock_ms); tick(r);
}
static int log_message(const char *format, ...)
{
    va_list args; va_start(args, format);
    int result = capture(format, args); va_end(args); return result;
}
__declspec(dllexport) int kms_run_keyboard_console_tests(void)
{
    static kms_router_t r;
    buffer = (kms_log_buffer_t){.records = records, .capacity = 128}; mutex = (void *)1;
    checks = clears = emitted = 0; fault = false; clock_ms = 0;
    kms_router_init(&r, NULL, NULL, NULL, NULL);
    r.stop_logs = kms_diagnostic_log_stop; r.console_input = kms_diagnostic_console_input;
    append_output("old log\n", 8); CHECK(kms_diagnostic_log_type(NULL)); r.diagnostic_typing = true;
    key(&r, 12); CHECK(interactive && r.diagnostic_typing && !fault && clears == 0);
    key(&r, 0);
    for (unsigned n = 0; n < 300; ++n) tick(&r);
    CHECK(emitted > 50 && r.diagnostic_typing && typing.cursor == buffer.end);
    // A quiet console must survive beyond the normal three-second typing timeout.
    for (unsigned n = 0; n < 500; ++n) tick(&r);
    CHECK(r.diagnostic_typing && interactive && clears == 0);
    // Input, editing, submission and continued output use production callbacks.
    static const unsigned keys[] = {11,8,27,42,15,19,40}; // h e x Backspace l p Enter
    for (unsigned n = 0; n < sizeof(keys)/sizeof(keys[0]); ++n) { key(&r, keys[n]); key(&r, 0); }
    char line[KMS_COMMAND_BYTES]; CHECK(kms_diagnostic_console_command(line, sizeof(line)) && !strcmp(line, "help"));
    CHECK(!kms_diagnostic_console_command(line, sizeof(line)));
    __wrap_fwrite("worker result\n", 1, 14, stdout);
    for (unsigned n = 0; n < 200; ++n) tick(&r);
    CHECK(r.diagnostic_typing && typing.cursor == buffer.end && clears == 0);
    // Escape interrupts a response, consumes held/repeated Escape, and ignores late output.
    __wrap_fwrite("a long response to interrupt\n", 1, 29, stdout); tick(&r);
    CHECK(typing.release);
    key(&r, 41); CHECK(interactive && r.diagnostic_typing && output_paused && clears == 0);
    unsigned stopped_at = emitted;
    key(&r, 41); key(&r, 0);
    __wrap_fwrite("late worker response\n", 1, 21, stdout);
    for (unsigned n = 0; n < 400; ++n) tick(&r);
    CHECK(interactive && r.diagnostic_typing && emitted == stopped_at && clears == 0);
    // A fresh command resumes echo/output and is submitted normally.
    key(&r, 4); key(&r, 0); key(&r, 40); key(&r, 0);
    CHECK(kms_diagnostic_console_command(line, sizeof(line)) && !strcmp(line, "a"));
    for (unsigned n = 0; n < 50; ++n) tick(&r);
    CHECK(!output_paused && emitted > stopped_at);
    key(&r, 41); CHECK(!interactive && !r.diagnostic_typing && clears == 1);
    CHECK(kms_key(&r.inputs[0].blocked, 41));
    // A full 64-byte batch followed by exactly one final character, no newline.
    fault = false; batched = true; available = true;
    kms_router_init(&r, NULL, NULL, NULL, NULL);
    r.stop_logs = kms_diagnostic_log_stop; r.console_input = kms_diagnostic_console_input;
    buffer.first = buffer.end = 0;
    char tail[KMS_TEXT_BYTES + 1]; memset(tail, 'a', sizeof(tail));
    append_output(tail, sizeof(tail));
    CHECK(kms_diagnostic_log_type(NULL)); r.diagnostic_typing = true;
    tick(&r); CHECK(batches == 1 && last_batch == KMS_TEXT_BYTES && batch_pending);
    tick(&r); CHECK(batches == 1 && r.diagnostic_typing); // Wait for receiver completion.
    batch_pending = false; tick(&r);
    CHECK(batches == 2 && last_batch == 1 && batch_pending && r.diagnostic_typing);
    tick(&r); CHECK(r.diagnostic_typing);
    batch_pending = false; tick(&r); CHECK(!r.diagnostic_typing);
    // Interactive single-character echo is sent immediately while otherwise idle.
    fault = false; CHECK(kms_diagnostic_log_type(NULL)); r.diagnostic_typing = true;
    key(&r, 12); key(&r, 0); batch_pending = false;
    for (unsigned n = 0; n < 5; ++n) { tick(&r); batch_pending = false; }
    unsigned before = batches;
    key(&r, 4); key(&r, 0);
    CHECK(batches == before + 1 && last_batch == 1 && batch_pending);
    unsigned cancellations = text_cancels;
    key(&r, 41); CHECK(text_cancels == cancellations + 1 && output_paused && !batch_pending);
    // UART can be blocked for longer than the ACK deadline. Neither a normal
    // log nor a saturated live-output queue may make its producer wait for it.
    live_output = (void *)3; uart_delay = 100; uart_calls = live_count = live_head = queue_waits = 0;
    uint32_t began = clock_ms; uint64_t retained = buffer.end;
    CHECK(log_message("radio fault\n") == 12);
    CHECK(log_message("radio recovered\n") == 16);
    CHECK(log_message("another fault\n") == 14);
    CHECK(clock_ms == began && !uart_calls && !queue_waits && live_count == 2);
    CHECK(buffer.end == retained + 3 && atomic_load(&live_dropped) == 1);
    CHECK(live_records[0].length == 12 && !memcmp(live_records[0].text, "radio fault\n", 12));
    CHECK(drain_live() && uart_calls == 1 && clock_ms == began + 100 && live_count == 1);
    CHECK(drain_live() && uart_calls == 2 && clock_ms == began + 200 && !live_count);
    kms_log_record_t last;
    CHECK(kms_log_buffer_read(&buffer, buffer.end - 1, &last) && last.length == 14);
    CHECK(!memcmp(last.text, "another fault\n", 14));
    char long_line[600]; memset(long_line, 'a', sizeof(long_line)); long_line[599] = 0;
    CHECK(log_message(long_line) == 599 && live_count == 1);
    CHECK(live_records[live_head % 2].length == KMS_LOG_TEXT_BYTES);
    CHECK(live_records[live_head % 2].text[KMS_LOG_TEXT_BYTES - 1] == '\n');
    return 0;
}
__declspec(dllexport) unsigned kms_keyboard_console_checks(void) { return checks; }
