#include "kms_log_buffer.h"
#include "kms_core.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks, starts, stops, leaked;
static bool start_ok, console_active;
static bool console_input(void *ctx, const kms_state_t *previous, const kms_state_t *state)
{
    (void)ctx;
    if (kms_key(state, 41) && !kms_key(previous, 41)) return false;
    if (kms_key(state, 12) && !kms_key(previous, 12)) console_active = true;
    return console_active;
}
static bool start(void *ctx) { (void)ctx; ++starts; return start_ok; }
static void stop(void *ctx) { (void)ctx; ++stops; }
static void emit(void *ctx, const kms_state_t *s, int32_t x, int32_t y, int32_t w, int32_t p)
{
    (void)ctx; (void)x; (void)y; (void)w; (void)p;
    if (kms_key(s, KMS_HID_KEY_L)) ++leaked;
}
static void key(kms_router_t *r, unsigned input, unsigned usage, uint32_t now)
{
    kms_sample_t s = {0};
    if (usage >= 0xe0) s.state.modifiers = (uint8_t)(1u << (usage - 0xe0));
    else if (usage) kms_set_key(&s.state, usage, true);
    kms_router_input(r, input, &s, now);
}
__declspec(dllexport) int kms_run_log_tests(void)
{
    checks = starts = stops = leaked = 0; start_ok = true;
    kms_log_record_t records[3], out;
    kms_log_buffer_t b = {.records = records, .capacity = 3};
    kms_log_buffer_append(&b, 10, "first\n", 6);
    kms_log_buffer_append(&b, 20, "second\n", 7);
    uint64_t end = b.end;
    kms_log_buffer_append(&b, 30, "third\n", 6);
    CHECK(end == 2 && b.end == 3 && kms_log_buffer_read(&b, 0, &out));
    CHECK(out.at == 10 && out.length == 6 && !memcmp(out.text, "first\n", 6));
    kms_log_buffer_append(&b, 40, "fourth\n", 7);
    CHECK(b.evicted == 1 && b.first == 1 && !kms_log_buffer_read(&b, 0, &out));
    CHECK(kms_log_buffer_read(&b, 1, &out) && out.at == 20);
    kms_log_buffer_expire(&b, KMS_LOG_KEEP_MS + 20);
    CHECK(b.first == 2 && b.evicted == 1);
    kms_log_buffer_expire(&b, KMS_LOG_KEEP_MS + 40);
    CHECK(b.first == b.end);
    kms_log_buffer_append(&b, UINT32_MAX - 5, "wrap", 4);
    kms_log_buffer_expire(&b, 10);
    CHECK(b.end - b.first == 1);
    kms_log_buffer_expire(&b, KMS_LOG_KEEP_MS);
    CHECK(b.first == b.end);
    char large[600]; memset(large, 'x', sizeof(large));
    kms_log_buffer_append(&b, KMS_LOG_KEEP_MS + 1, large, sizeof(large));
    CHECK(b.truncated == 1 && kms_log_buffer_read(&b, b.first, &out));
    CHECK(out.length == KMS_LOG_TEXT_BYTES && !memcmp(out.text + KMS_LOG_TEXT_BYTES - 13, " [truncated]\n", 13));
    kms_log_buffer_append(&b, KMS_LOG_KEEP_MS + 2, large, KMS_LOG_TEXT_BYTES);
    CHECK(b.truncated == 1 && kms_log_buffer_read(&b, b.end - 1, &out));
    CHECK(out.text[KMS_LOG_TEXT_BYTES - 1] == 'x');
    uint8_t usage, modifiers;
    CHECK(kms_log_key('a', false, &usage, &modifiers) && usage == 4 && !modifiers);
    CHECK(kms_log_key('A', false, &usage, &modifiers) && usage == 4 && modifiers == 2);
    CHECK(kms_log_key('a', true, &usage, &modifiers) && usage == 4 && modifiers == 2);
    CHECK(kms_log_key('A', true, &usage, &modifiers) && usage == 4 && !modifiers);
    CHECK(kms_log_key('\n', false, &usage, &modifiers) && usage == 40 && !modifiers);
    CHECK(!kms_log_key('\r', false, &usage, &modifiers));
    CHECK(kms_log_key('\x1b', false, &usage, &modifiers) && usage == 56 && modifiers == 2);
    CHECK(kms_log_key('%', false, &usage, &modifiers) && usage == 34 && modifiers == 2);
    CHECK(kms_log_key(':', false, &usage, &modifiers) && usage == 51 && modifiers == 2);
    for (unsigned c = 32; c < 127; ++c) CHECK(kms_log_key((char)c, false, &usage, &modifiers) && usage >= 4 && usage <= 56);
    static const unsigned triggers[] = {0xe0, 0xe2, 0xe1, KMS_HID_KEY_SCROLL_LOCK,
        KMS_HID_KEY_INSERT, KMS_HID_KEY_PAUSE, KMS_HID_KEY_NUM_LOCK};
    for (unsigned t = 0; t < 7; ++t) for (unsigned input = 0; input < 2; ++input)
        for (unsigned overlap = 0; overlap < 3; ++overlap) {
            static kms_router_t r;
            kms_router_init(&r, emit, NULL, NULL, NULL); r.dump_logs = start; r.stop_logs = stop;
            CHECK(kms_router_set_trigger(&r, (kms_trigger_t)(t + 1)));
            unsigned previous_starts = starts, previous_stops = stops;
            key(&r, input, triggers[t], 100); key(&r, input, 0, 120);
            key(&r, input, triggers[t], 140);
            if (!overlap) key(&r, input, 0, 160);
            kms_sample_t s = {0}; kms_set_key(&s.state, KMS_HID_KEY_L, true);
            if (overlap == 1) {
                if (triggers[t] >= 0xe0) s.state.modifiers = (uint8_t)(1u << (triggers[t] - 0xe0));
                else kms_set_key(&s.state, triggers[t], true);
            }
            kms_router_input(&r, input, &s, 180);
            CHECK(r.diagnostic_typing && starts == previous_starts + 1 && !leaked);
            key(&r, input, 0, 200);
            CHECK(r.diagnostic_typing && stops == previous_stops);
            // Movement never cancels; a fresh key from either keyboard does.
            s = (kms_sample_t){.x = 2}; kms_router_input(&r, 1 - input, &s, 210);
            CHECK(r.diagnostic_typing);
            key(&r, 1 - input, 4, 220);
            CHECK(!r.diagnostic_typing && stops == previous_stops + 1 && !leaked);
            CHECK(kms_key(&r.inputs[1 - input].blocked, 4));
        }
    static kms_router_t r;
    kms_router_init(&r, emit, NULL, NULL, NULL); r.dump_logs = start; r.stop_logs = stop;
    kms_router_set_trigger(&r, KMS_TRIGGER_INSERT); start_ok = false;
    key(&r, 0, KMS_HID_KEY_INSERT, 100); key(&r, 0, 0, 120);
    key(&r, 0, KMS_HID_KEY_INSERT, 140); key(&r, 0, 0, 160); key(&r, 0, KMS_HID_KEY_L, 180);
    CHECK(!r.diagnostic_typing && kms_router_take_notice(&r) == KMS_NOTICE_ERROR && !leaked);
    // A modifier, media key and system key on another input also stop output.
    for (unsigned kind = 0; kind < 3; ++kind) {
        kms_router_init(&r, emit, NULL, NULL, NULL); r.stop_logs = stop; r.diagnostic_typing = true;
        kms_sample_t s = {0}; unsigned previous_stops = stops;
        if (kind == 0) s.state.modifiers = 2;
        else if (kind == 1) s.state.consumer[0] = 0xe9;
        else s.state.system = 1;
        kms_router_input(&r, 1, &s, 300);
        CHECK(!r.diagnostic_typing && stops == previous_stops + 1);
        CHECK(kms_state_equal(&r.inputs[1].blocked, &s.state));
    }
    // Interactive callback can consume I and commands; Escape still cancels and blocks held keys.
    kms_router_init(&r, emit, NULL, NULL, NULL);
    r.stop_logs = stop; r.console_input = console_input; r.diagnostic_typing = true;
    console_active = false;
    key(&r, 0, 12, 400);
    CHECK(console_active && r.diagnostic_typing);
    key(&r, 0, 0, 410); key(&r, 1, 4, 420);
    CHECK(r.diagnostic_typing && kms_key(&r.inputs[1].blocked, 4));
    key(&r, 1, 0, 430); key(&r, 0, 41, 440);
    CHECK(!r.diagnostic_typing && kms_key(&r.inputs[0].blocked, 41));
    CHECK(kms_log_key('\b', false, &usage, &modifiers) && usage == 42 && !modifiers);
    return 0;
}
__declspec(dllexport) int kms_log_test_count(void) { return checks; }
