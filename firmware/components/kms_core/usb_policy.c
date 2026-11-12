#include "kms_usb_policy.h"
#include <string.h>

static bool same_device(const kms_usb_rule_t *a, const kms_usb_rule_t *b)
{
    return a->vid == b->vid && a->pid == b->pid && a->serial == b->serial &&
           a->interface_number == b->interface_number;
}
void kms_usb_policy_reset(kms_usb_policy_t *p, bool automatic)
{
    memset(p, 0, sizeof(*p)); p->schema = 1; p->automatic = automatic;
}
bool kms_usb_policy_valid(const kms_usb_policy_t *p)
{
    if (p->schema != 1 || p->automatic > 1 || p->count > KMS_USB_RULES ||
        p->reserved[0] || p->reserved[1]) return false;
    for (unsigned n = 0; n < p->count; ++n) {
        const kms_usb_rule_t *r = &p->rules[n];
        if (r->enabled > 1 || r->reserved[0] || r->reserved[1]) return false;
        for (unsigned j = 0; j < n; ++j) if (same_device(r, &p->rules[j])) return false;
    }
    return true;
}
bool kms_usb_policy_enabled(const kms_usb_policy_t *p, const kms_usb_rule_t *key)
{
    for (unsigned n = 0; n < p->count; ++n)
        if (same_device(&p->rules[n], key)) return p->rules[n].enabled != 0;
    return p->automatic != 0;
}
bool kms_usb_policy_set(kms_usb_policy_t *p, kms_usb_rule_t rule)
{
    if (rule.enabled > 1) return false;
    rule.reserved[0] = rule.reserved[1] = 0;
    for (unsigned n = 0; n < p->count; ++n) if (same_device(&p->rules[n], &rule)) {
        p->rules[n] = rule; return true;
    }
    if (p->count == KMS_USB_RULES) return false;
    p->rules[p->count++] = rule; return true;
}
static bool usage(const kms_hid_field_t *f, unsigned first, unsigned last)
{
    if (!f->usage_count) return f->first <= last && f->last >= first;
    for (unsigned n = 0; n < f->usage_count; ++n)
        if (f->usages[n] >= first && f->usages[n] <= last) return true;
    return false;
}
uint8_t kms_usb_hid_kinds(const kms_hid_parser_t *p)
{
    uint8_t kinds = 0;
    for (unsigned n = 0; n < p->count; ++n) {
        const kms_hid_field_t *f = &p->fields[n];
        if (f->page == 7) kinds |= KMS_USB_KEYBOARD;
        if (f->page == 9 || (f->page == 1 && (f->flags & 4) &&
            (usage(f, 0x30, 0x31) || usage(f, 0x38, 0x38)))) kinds |= KMS_USB_MOUSE;
        if (f->page == 0x0c) kinds |= KMS_USB_CONSUMER;
        if (f->page == 1 && usage(f, 0x81, 0x83)) kinds |= KMS_USB_SYSTEM;
    }
    for (unsigned id = 0; id < 256; ++id)
        if (kms_hid_led_report_supported(p, id)) { kinds |= KMS_USB_LEDS; break; }
    return kinds;
}

int kms_usb_policy_override(const kms_usb_policy_t *p, const kms_usb_rule_t *key)
{
    for (unsigned n = 0; n < p->count; ++n)
        if (same_device(&p->rules[n], key)) return p->rules[n].enabled != 0;
    return -1;
}
uint32_t kms_usb_plan(const kms_usb_candidate_t *candidates, unsigned count, unsigned baseline)
{
    if (count > KMS_USB_RULES || baseline >= KMS_USB_CHANNELS) return 0;
    const unsigned input_kinds = KMS_USB_KEYBOARD | KMS_USB_MOUSE | KMS_USB_CONSUMER | KMS_USB_SYSTEM;
    typedef struct {
        uint32_t mask;
        unsigned cost, kinds;
        uint8_t device;
        bool enabled, preferred, active;
    } bundle_t;
    bundle_t bundles[KMS_USB_RULES] = {0}; unsigned groups = 0;
    for (unsigned n = 0; n < count; ++n) {
        const kms_usb_candidate_t *c = &candidates[n];
        if (!(c->kinds & input_kinds) || !c->endpoints) continue;
        unsigned group;
        for (group = 0; group < groups; ++group) if (bundles[group].device == c->device) break;
        if (group == groups) bundles[groups++] = (bundle_t){.device = c->device, .enabled = true};
        bundle_t *b = &bundles[group];
        b->mask |= 1u << n; b->cost += c->endpoints; b->kinds |= c->kinds;
        b->enabled &= c->enabled; b->preferred |= c->preferred; b->active |= c->active;
    }
    uint32_t best_mask = 0; int32_t best_score = -1; unsigned best_cost = KMS_USB_CHANNELS;
    unsigned budget = KMS_USB_CHANNELS - baseline;
    // Keep all supported inputs of a physical device polled together. A receiver
    // can share an internal queue between keyboard and pointer endpoints; leaving
    // one endpoint unpolled can freeze another when its peripheral sends input.
    for (unsigned a = 0; a < groups; ++a) for (unsigned b = a; b < groups; ++b) {
        const bundle_t *first = &bundles[a], *second = &bundles[b];
        if (!first->enabled || !second->enabled) continue;
        unsigned cost = first->cost + (a == b ? 0 : second->cost);
        if (cost > budget) continue;
        unsigned kinds = first->kinds | second->kinds;
        unsigned devices = a == b ? 1 : 2;
        unsigned coverage = !!(kinds & KMS_USB_KEYBOARD) + !!(kinds & KMS_USB_MOUSE);
        unsigned preferred = first->preferred + (a == b ? 0 : second->preferred);
        unsigned active = first->active + (a == b ? 0 : second->active);
        int32_t score = preferred * 10000000 + coverage * 1000000 + devices * 100000 +
            ((kinds & (KMS_USB_CONSUMER | KMS_USB_SYSTEM | KMS_USB_LEDS)) ? 10 : 0) + active;
        if (devices == 1 && cost < budget) score += 5000;
        if (score > best_score || (score == best_score && cost < best_cost)) {
            best_score = score; best_mask = first->mask | second->mask; best_cost = cost;
        }
    }
    return best_mask;
}
