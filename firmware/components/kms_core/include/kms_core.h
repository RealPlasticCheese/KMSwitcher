#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kms_settings.h"
#include "kms_protocol.h"

typedef struct {
    uint8_t keys[KMS_KEY_BYTES];
    uint8_t modifiers;
    uint16_t buttons;
    uint16_t consumer[KMS_CONSUMERS];
    uint8_t system;
} kms_state_t;

typedef struct {
    kms_state_t state;
    int32_t x, y, wheel, pan;
    uint8_t report_id;
} kms_sample_t;

struct kms_key_trace;
typedef void (*kms_key_trace_fn)(void *, const struct kms_key_trace *);

static inline bool kms_key(const kms_state_t *s, unsigned key)
{ return key < 256 && (s->keys[key / 8] & (1u << (key % 8))) != 0; }
static inline void kms_set_key(kms_state_t *s, unsigned key, bool down)
{
    if (key < 256) {
        if (down) s->keys[key / 8] |= (uint8_t)(1u << (key % 8));
        else s->keys[key / 8] &= (uint8_t)~(1u << (key % 8));
    }
}
bool kms_state_equal(const kms_state_t *a, const kms_state_t *b);
void kms_state_merge(kms_state_t *into, const kms_state_t *from);

typedef struct {
    uint16_t offset, count, page, first, last;
    int32_t logical_min;
    uint16_t usages[KMS_HID_USAGES];
    uint8_t usage_count, bits, report_id, flags;
} kms_hid_field_t;
typedef struct {
    kms_hid_field_t led_fields[KMS_HID_LED_FIELDS];
    uint16_t input_bits[256];
    uint16_t output_bits[256];
    uint8_t unsafe_output[32];
    uint8_t count, led_count;
    bool report_ids;
    // Keep input fields last: after parsing, heap users can release the unused
    // tail. Parsing always requires a fresh, full-sized workspace; retained
    // parsers are read only and contain exactly `count` input fields.
    kms_hid_field_t fields[KMS_HID_FIELDS];
} kms_hid_parser_t;
static inline size_t kms_hid_parser_storage_size(const kms_hid_parser_t *p)
{ return offsetof(kms_hid_parser_t, fields) + p->count * sizeof(p->fields[0]); }
bool kms_hid_parse(kms_hid_parser_t *p, const uint8_t *descriptor, size_t len);
bool kms_hid_decode(const kms_hid_parser_t *p, const uint8_t *data, size_t len,
                    kms_sample_t *sample);
bool kms_hid_led_report_supported(const kms_hid_parser_t *, uint8_t id);
// Encode the entire safe LED Output report, including its ID prefix if present.
size_t kms_hid_led_report(const kms_hid_parser_t *, uint8_t id, uint8_t leds,
                          uint8_t *data, size_t capacity);

typedef void (*kms_emit_fn)(void *ctx, const kms_state_t *, int32_t, int32_t, int32_t, int32_t);
typedef bool (*kms_select_fn)(void *ctx, unsigned slot);
typedef void (*kms_pair_fn)(void *ctx, unsigned slot);
typedef enum {
    KMS_TRIGGER_CTRL = 1, KMS_TRIGGER_ALT, KMS_TRIGGER_SHIFT,
    KMS_TRIGGER_SCROLL_LOCK, KMS_TRIGGER_INSERT, KMS_TRIGGER_PAUSE, KMS_TRIGGER_NUM_LOCK
} kms_trigger_t;
_Static_assert(KMS_TRIGGER_DEFAULT >= KMS_TRIGGER_CTRL && KMS_TRIGGER_DEFAULT <= KMS_TRIGGER_NUM_LOCK,
               "Default trigger must be one of the supported choices");
// Persisted user preferences use an explicit versioned codec, never a C struct blob.
enum { KMS_PREFERENCES_BYTES = 32 };
typedef struct {
    uint32_t double_ms, select_ms;
    uint16_t jiggler_seconds;
    uint8_t jiggler_distance;
    uint8_t trigger, normal_led, brightness, keyboard_leds, boot_slot; // 0: automatic, 1-7: wait for this dongle.
    uint8_t slot_colors[KMS_SLOTS];
} kms_user_preferences_t;
void kms_user_preferences_defaults(kms_user_preferences_t *);
bool kms_user_preferences_valid(const kms_user_preferences_t *);
bool kms_user_preferences_encode(const kms_user_preferences_t *, uint8_t *);
bool kms_user_preferences_decode(kms_user_preferences_t *, const uint8_t *, size_t);

typedef bool (*kms_configure_fn)(void *ctx, kms_trigger_t trigger);
enum kms_router_mode { KMS_MODE_NORMAL, KMS_MODE_SELECT, KMS_MODE_PAIR, KMS_MODE_CONFIG, KMS_MODE_DEFAULT };
enum kms_notice { KMS_NOTICE_NONE, KMS_NOTICE_SAVED, KMS_NOTICE_CANCELLED, KMS_NOTICE_RESTORED, KMS_NOTICE_ERROR };
typedef struct {
    uint32_t sequence;
    uint8_t leds;
    bool valid;
} kms_led_state_t;
void kms_led_state_set(kms_led_state_t *, bool valid, uint8_t leds);

typedef struct {
    kms_state_t physical, visible, blocked;
    uint32_t deadline;
    uint8_t tap_phase, tap_modifier;
    uint8_t trace_consumed_reason;
    bool tap_replay_blocked;
} kms_input_t;
typedef struct {
    kms_input_t inputs[KMS_INPUTS];
    kms_state_t trace_output;
    kms_key_trace_fn trace;
    kms_emit_fn emit;
    kms_select_fn select;
    kms_pair_fn pair;
    kms_configure_fn configure;
    kms_select_fn save_boot_slot;
    bool (*toggle_jiggler)(void *ctx);
    bool (*cycle_jiggler_mode)(void *ctx);
    bool (*dump_logs)(void *ctx);
    void (*stop_logs)(void *ctx);
    bool (*console_input)(void *ctx, const kms_state_t *previous, const kms_state_t *state);
    bool diagnostic_typing;
    void *ctx;
    uint32_t deadline;
    uint8_t mode, owner, tap_modifier;
    kms_trigger_t trigger, candidate;
    uint32_t double_ms, select_ms;
    enum kms_notice notice;
    bool config_wait_release;
} kms_router_t;
void kms_router_init(kms_router_t *, kms_emit_fn, kms_select_fn, kms_pair_fn, void *);
bool kms_trigger_valid(unsigned trigger);
const char *kms_trigger_name(kms_trigger_t trigger);
bool kms_router_set_trigger(kms_router_t *, kms_trigger_t);
bool kms_router_set_preferences(kms_router_t *, const kms_user_preferences_t *);
enum kms_notice kms_router_take_notice(kms_router_t *);
void kms_router_input(kms_router_t *, unsigned input, const kms_sample_t *, uint32_t now);
void kms_router_tick(kms_router_t *, uint32_t now);
void kms_router_remove(kms_router_t *, unsigned input);
// Block held PC output after delivery loss, retaining local shortcut progress.
void kms_router_block_output(kms_router_t *);
// Physical input discontinuity or explicit mode/route change cancels shortcuts.
void kms_router_block_held(kms_router_t *);

// Hardware-independent button gesture handling; all durations come from settings.
enum kms_button_event { KMS_BUTTON_NONE, KMS_BUTTON_LONG, KMS_BUTTON_RESTORE_TRIGGER, KMS_BUTTON_FACTORY_RESET };
typedef struct {
    uint32_t started, changed, pressed, click_started;
    unsigned clicks;
    bool controller, raw, stable, long_fired, factory_armed, factory_ready;
} kms_button_t;
void kms_button_init(kms_button_t *, bool controller, uint32_t now);
enum kms_button_event kms_button_update(kms_button_t *, bool down, uint32_t now);
bool kms_notice_color(uint32_t elapsed, unsigned color, unsigned flashes, unsigned *shown);

enum kms_type {
    KMS_STATUS = 1, KMS_ACTIVATE, KMS_INPUT, KMS_RELEASE, KMS_ACK,
    KMS_PAIR_REQUEST, KMS_PAIR_OFFER, KMS_PAIR_CONFIRM, KMS_PAIR_DONE, KMS_JIGGLER,
    KMS_TEXT, KMS_TEXT_ACK, KMS_INPUT_BATCH
};
typedef struct { kms_state_t state; uint32_t motion[4]; } kms_packet_input_t;
// Fixed packets share the prefix of the batch-capable working packet. The
// anonymous view keeps the same field names in both the codec and callers.
#define KMS_SINGLE_PACKET_FIELDS \
    uint8_t type, slot, flags; \
    uint32_t session, receiver, generation, sequence; \
    kms_state_t state; \
    uint32_t motion[4]; \
    uint8_t public_key[32], nonce[16]; \
    kms_led_state_t keyboard_leds;
typedef struct { KMS_SINGLE_PACKET_FIELDS } kms_single_packet_t;
typedef struct {
    union {
        kms_single_packet_t single;
        struct { KMS_SINGLE_PACKET_FIELDS };
    };
    // The first batch input uses state/motion above. Pairing and legacy packets
    // retain their exact v2 representation; batches use a negotiated extension.
    uint8_t batch_count;
    kms_packet_input_t extra[KMS_INPUT_BATCH_MAX - 1];
} kms_packet_t;
#undef KMS_SINGLE_PACKET_FIELDS
size_t kms_wire_encode(const kms_packet_t *, uint8_t *out, size_t capacity);
bool kms_wire_decode(kms_packet_t *, const uint8_t *in, size_t len);
void kms_batch_advertise(kms_packet_t *status);
unsigned kms_batch_limit(const kms_packet_t *status);
size_t kms_batch_wire_size(const kms_packet_t *);
static inline bool kms_packet_is_input(const kms_packet_t *p)
{ return p->type == KMS_INPUT || p->type == KMS_INPUT_BATCH; }
bool kms_newer(uint32_t a, uint32_t b);
// Intervals must be less than half the 32-bit clock range. A timestamp sampled
// before an event was processed is not an elapsed timeout; clock wrap is valid.
bool kms_time_elapsed(uint32_t now, uint32_t since, uint32_t duration);

typedef struct { kms_led_state_t state; bool received; } kms_led_feedback_t;
// STATUS/ACK must belong to the selected receiver, current master boot and route.
// The caller also verifies the sender MAC; duplicates and reordered LED versions
// cannot replace the current state. Reset this tracker on every route change.
bool kms_led_feedback_accept(kms_led_feedback_t *, const kms_packet_t *,
                              uint32_t master_boot, uint32_t receiver_boot,
                              unsigned slot, uint32_t generation);

typedef struct {
    uint32_t session, generation, sequence, last_seen, motion[4];
    bool active, initialized;
} kms_receiver_guard_t;
// A duplicate gets acknowledged but is not delivered twice. Activation is neutral.
bool kms_receiver_accept(kms_receiver_guard_t *, const kms_packet_t *, uint32_t now,
                         kms_sample_t *out, bool *deliver);
// The caller commits the copied guard only after all returned samples fit its
// output queue. A repeated batch is acknowledged without replaying any sample.
bool kms_receiver_accept_many(kms_receiver_guard_t *, const kms_packet_t *, uint32_t now,
                              kms_sample_t out[KMS_INPUT_BATCH_MAX], unsigned *count, bool *deliver);
bool kms_receiver_expire(kms_receiver_guard_t *, uint32_t now);
