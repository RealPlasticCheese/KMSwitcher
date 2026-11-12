#include "kms_core.h"
#include <string.h>

typedef struct {
    uint32_t page, size, count;
    int32_t min, max;
    uint8_t id;
} global_t;

static uint32_t value(const uint8_t *s, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint32_t)s[i] << (8 * i);
    return v;
}
static int32_t signed_value(uint32_t v, unsigned bits)
{
    if (bits && bits < 32 && (v & (1u << (bits - 1)))) v |= ~((1u << bits) - 1u);
    return (int32_t)v;
}

bool kms_hid_parse(kms_hid_parser_t *p, const uint8_t *d, size_t len)
{
    if (!p || !d || !len || len > KMS_DESCRIPTOR_BYTES) return false;
    memset(p, 0, sizeof(*p));
    global_t g = {0}, stack[KMS_HID_GLOBAL_DEPTH];
    unsigned sp = 0, depth = 0, n = 0;
    bool application[KMS_HID_COLLECTION_DEPTH] = {0};
    bool keyboard[KMS_HID_COLLECTION_DEPTH] = {0};
    uint16_t usages[KMS_HID_USAGES] = {0}, first = 0, last = 0;
    bool range = false;
    for (size_t i = 0; i < len;) {
        uint8_t tag = d[i++];
        if (tag == 0xfe) return false; // bounded short-item parser
        unsigned bytes = (tag & 3) == 3 ? 4 : tag & 3;
        if (len - i < bytes) return false;
        uint32_t v = value(d + i, bytes);
        i += bytes;
        switch (tag & 0xfc) {
        case 0x04: if (v > 0xffff) return false; g.page = v; break;
        case 0x14: g.min = signed_value(v, bytes * 8); break;
        case 0x24: g.max = g.min < 0 ? signed_value(v, bytes * 8) : (int32_t)v; break;
        case 0x74: if (v > 32) return false; g.size = v; break;
        case 0x84: if (!v || v > 255) return false; g.id = (uint8_t)v; p->report_ids = true; break;
        case 0x94: if (v > 2048) return false; g.count = v; break;
        case 0xa4: if (sp == KMS_HID_GLOBAL_DEPTH) return false; stack[sp++] = g; break;
        case 0xb4: if (!sp) return false; g = stack[--sp]; break;
        case 0x08:
            if (v > 0xffff || n == KMS_HID_USAGES) return false;
            usages[n++] = (uint16_t)v;
            break;
        case 0x18: if (v > 0xffff) return false; first = (uint16_t)v; range = true; break;
        case 0x28: if (v > 0xffff) return false; last = (uint16_t)v; break;
        case 0x80: {
            uint32_t bits = g.size * g.count;
            if (!g.size || !g.count || bits > KMS_REPORT_BYTES * 8u ||
                p->input_bits[g.id] + bits > KMS_REPORT_BYTES * 8u || (range && last < first)) return false;
            bool supported = depth && application[depth - 1] &&
                             (g.page == 7 || g.page == 9 || g.page == 0x0c || g.page == 1);
            if (!(v & 1) && supported && (n || range)) {
                if (p->count == KMS_HID_FIELDS) return false;
                kms_hid_field_t *f = &p->fields[p->count++];
                f->offset = p->input_bits[g.id]; f->count = (uint16_t)g.count;
                f->bits = (uint8_t)g.size; f->report_id = g.id;
                f->flags = (uint8_t)v; f->page = (uint16_t)g.page;
                f->logical_min = g.min; f->first = first; f->last = last;
                f->usage_count = (uint8_t)n;
                memcpy(f->usages, usages, n * sizeof(uint16_t));
            }
            p->input_bits[g.id] += (uint16_t)bits;
            break;
        }
        case 0x90: { // Output offsets are independent of Input offsets for each ID.
            uint32_t bits = g.size * g.count, end = p->output_bits[g.id] + bits;
            bool safe = g.size && g.count && end <= KMS_REPORT_BYTES * 8u;
            if (!(v & 1)) {
                safe = safe && depth && keyboard[depth - 1] && g.page == 8 &&
                       (v & 2) && !(v & (4 | 0x100)) && g.min == 0 && g.max == 1 &&
                       (n || range) && (!range || last >= first) && p->led_count < KMS_HID_LED_FIELDS;
                if (safe) {
                    kms_hid_field_t *f = &p->led_fields[p->led_count];
                    *f = (kms_hid_field_t){.offset = p->output_bits[g.id], .count = (uint16_t)g.count,
                        .bits = (uint8_t)g.size, .report_id = g.id, .first = first, .last = last,
                        .usage_count = (uint8_t)n};
                    memcpy(f->usages, usages, n * sizeof(uint16_t));
                    for (unsigned j = 0; j < g.count; ++j) {
                        unsigned usage = n ? usages[j < n ? j : n - 1] :
                            (first + j <= last ? first + j : last);
                        if (usage < 1 || usage > 5) { safe = false; break; }
                    }
                    if (safe) ++p->led_count;
                }
            }
            // Never zero-fill vendor data or unsupported controls sharing an LED ID.
            if (!safe) p->unsafe_output[g.id / 8] |= 1u << (g.id % 8);
            p->output_bits[g.id] = end <= KMS_REPORT_BYTES * 8u ? (uint16_t)end : UINT16_MAX;
            break;
        }
        case 0xa0: {
            if (depth == KMS_HID_COLLECTION_DEPTH) return false;
            bool allowed = depth ? application[depth - 1] : false;
            bool kbd = depth ? keyboard[depth - 1] : false;
            if (v == 1) { // Application collection: ignore gamepads and vendor functions.
                unsigned usage = n ? usages[0] : first;
                allowed = (g.page == 1 && (usage == 2 || usage == 6 || usage == 7 || usage == 0x80)) ||
                          (g.page == 0x0c && usage == 1);
                kbd = g.page == 1 && (usage == 6 || usage == 7);
            }
            keyboard[depth] = kbd; application[depth++] = allowed; break;
        }
        case 0xc0: if (!depth) return false; --depth; break;
        default: break;
        }
        if ((tag & 0x0c) == 0) { n = 0; first = last = 0; range = false; }
    }
    if (depth || sp || (p->report_ids && (p->input_bits[0] || p->output_bits[0]))) return false;
    if (p->count) return true;
    for (unsigned id = 0; id < 256; ++id) if (kms_hid_led_report_supported(p, id)) return true;
    return false;
}

static uint32_t bits_at(const uint8_t *data, unsigned offset, unsigned count)
{
    uint32_t v = 0;
    for (unsigned b = 0; b < count; ++b)
        v |= (uint32_t)((data[(offset + b) / 8] >> ((offset + b) % 8)) & 1) << b;
    return v;
}
static unsigned usage_at(const kms_hid_field_t *f, unsigned i)
{
    if (f->usage_count) return f->usages[i < f->usage_count ? i : f->usage_count - 1];
    return f->first + i <= f->last ? f->first + i : f->last;
}
bool kms_hid_led_report_supported(const kms_hid_parser_t *p, uint8_t id)
{
    if (!p || !p->output_bits[id] || (p->output_bits[id] + 7u) / 8u + (p->report_ids ? 1u : 0u) > KMS_REPORT_BYTES ||
        (p->unsafe_output[id / 8] & (1u << (id % 8)))) return false;
    for (unsigned n = 0; n < p->led_count; ++n) if (p->led_fields[n].report_id == id) return true;
    return false;
}
size_t kms_hid_led_report(const kms_hid_parser_t *p, uint8_t id, uint8_t leds,
                          uint8_t *data, size_t capacity)
{
    if (!data || !kms_hid_led_report_supported(p, id)) return 0;
    size_t prefix = p->report_ids ? 1u : 0u, bytes = (p->output_bits[id] + 7u) / 8u + prefix;
    if (bytes > capacity || bytes > KMS_REPORT_BYTES) return 0;
    memset(data, 0, bytes);
    if (prefix) *data++ = id;
    for (unsigned n = 0; n < p->led_count; ++n) {
        const kms_hid_field_t *f = &p->led_fields[n];
        if (f->report_id != id) continue;
        for (unsigned j = 0; j < f->count; ++j) if (leds & (1u << (usage_at(f, j) - 1))) {
            unsigned bit = f->offset + j * f->bits;
            data[bit / 8] |= 1u << (bit % 8); // Boolean 1, even for fields wider than one bit.
        }
    }
    return bytes;
}
static bool has_usage(const kms_hid_field_t *f, unsigned first, unsigned last)
{
    if (!f->usage_count) return f->first <= last && f->last >= first;
    for (unsigned i = 0; i < f->usage_count; ++i)
        if (f->usages[i] >= first && f->usages[i] <= last) return true;
    return false;
}
static bool supported_array(const kms_hid_field_t *f)
{
    if (f->page == 7 || f->page == 0x0c) return true;
    if (f->page == 9) return has_usage(f, 1, 16);
    if (f->page == 1)
        return has_usage(f, 0x81, 0x83) || ((f->flags & 4) &&
               (has_usage(f, 0x30, 0x31) || has_usage(f, 0x38, 0x38)));
    return false;
}
static bool consumer_add(kms_state_t *s, uint16_t usage)
{
    if (!usage) return true;
    for (unsigned i = 0; i < KMS_CONSUMERS; ++i) {
        if (s->consumer[i] == usage) return true;
        if (!s->consumer[i]) { s->consumer[i] = usage; return true; }
    }
    return false;
}

bool kms_hid_decode(const kms_hid_parser_t *p, const uint8_t *data, size_t len,
                    kms_sample_t *s)
{
    if (!p || !data || !s || !len) return false;
    memset(s, 0, sizeof(*s));
    uint8_t id = p->report_ids ? *data++ : 0;
    if (p->report_ids) --len;
    if (!p->input_bits[id] || len < (p->input_bits[id] + 7u) / 8u) return false;
    s->report_id = id;
    bool handled = false;
    for (unsigned j = 0; j < p->count; ++j) {
        const kms_hid_field_t *f = &p->fields[j];
        if (f->report_id != id) continue;
        // HID arrays report no asserted controls with an out-of-range selector,
        // commonly zero below a logical minimum of one (HID 1.11, 6.2.2.5).
        // The empty array is still a valid release for this report's source.
        if (!(f->flags & 2) && supported_array(f)) handled = true;
        for (unsigned i = 0; i < f->count; ++i) {
            uint32_t raw = bits_at(data, f->offset + i * f->bits, f->bits);
            int32_t v = f->logical_min < 0 ? signed_value(raw, f->bits) : (int32_t)raw;
            unsigned usage = usage_at(f, i);
            if (!(f->flags & 2)) { // array value is a usage selector
                if (v < f->logical_min) continue;
                uint32_t index = (uint32_t)v - (uint32_t)f->logical_min;
                if ((f->usage_count && index >= f->usage_count) ||
                    (!f->usage_count && index > (unsigned)(f->last - f->first))) continue;
                usage = usage_at(f, index);
                v = usage != 0;
            }
            if (f->page == 7) {
                handled = true;
                if (v && usage >= 1 && usage <= 3) return false; // rollover/error, preserve old state
                if (v && usage >= 0xe0 && usage <= 0xe7) s->state.modifiers |= 1u << (usage - 0xe0);
                else if (v && usage >= 4 && usage < 0xe0) kms_set_key(&s->state, usage, true);
            } else if (f->page == 9 && usage >= 1 && usage <= 16) {
                handled = true;
                if (v) s->state.buttons |= 1u << (usage - 1);
            } else if (f->page == 0x0c) {
                handled = true;
                if (usage == 0x238 && (f->flags & 4)) s->pan = v;
                else if (v && !consumer_add(&s->state, (uint16_t)usage)) return false;
            } else if (f->page == 1) {
                if ((usage == 0x30 || usage == 0x31 || usage == 0x38) && (f->flags & 4)) {
                    handled = true;
                    if (usage == 0x30) s->x = v;
                    else if (usage == 0x31) s->y = v;
                    else s->wheel = v;
                } else if (usage >= 0x81 && usage <= 0x83) {
                    handled = true;
                    if (v) s->state.system |= 1u << (usage - 0x81);
                }
            }
        }
    }
    return handled;
}
