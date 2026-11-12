#include "kms_usb_policy.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks;
__declspec(dllexport) unsigned kms_usb_policy_test_count(void) { return checks; }
__declspec(dllexport) int kms_run_usb_policy_tests(void)
{
    kms_usb_policy_t p;
    kms_usb_rule_t a = {.vid = 0x1241, .pid = 0x1203, .interface_number = 0};
    kms_usb_rule_t b = a; b.interface_number = 1;
    kms_usb_rule_t other_serial = a; other_serial.serial = 1234;
    kms_usb_policy_reset(&p, true);
    CHECK(kms_usb_policy_valid(&p));
    CHECK(kms_usb_policy_enabled(&p, &a));
    a.enabled = 0; CHECK(kms_usb_policy_set(&p, a));
    CHECK(!kms_usb_policy_enabled(&p, &a));
    CHECK(kms_usb_policy_enabled(&p, &b));
    CHECK(kms_usb_policy_enabled(&p, &other_serial));
    a.enabled = 1; CHECK(kms_usb_policy_set(&p, a));
    CHECK(p.count == 1 && kms_usb_policy_enabled(&p, &a));
    kms_usb_policy_t restored = p;
    CHECK(kms_usb_policy_valid(&restored) && kms_usb_policy_enabled(&restored, &a));
    kms_usb_policy_reset(&p, false);
    CHECK(!kms_usb_policy_enabled(&p, &a));
    CHECK(kms_usb_policy_set(&p, a) && kms_usb_policy_enabled(&p, &a));
    CHECK(!kms_usb_policy_enabled(&p, &b));
    CHECK(!kms_usb_policy_enabled(&p, &other_serial));
    for (unsigned n = 1; n < KMS_USB_RULES; ++n) {
        kms_usb_rule_t r = a; r.interface_number = n;
        CHECK(kms_usb_policy_set(&p, r));
    }
    kms_usb_rule_t extra = a; extra.serial = 789;
    restored = p;
    CHECK(!kms_usb_policy_set(&p, extra) && !memcmp(&p, &restored, sizeof(p)));
    a.enabled = 0; CHECK(kms_usb_policy_set(&p, a) && p.count == KMS_USB_RULES);
    CHECK(kms_usb_policy_valid(&p));
    p.rules[1] = p.rules[0]; CHECK(!kms_usb_policy_valid(&p));
    p = restored; p.schema = 2; CHECK(!kms_usb_policy_valid(&p));
    p = restored; p.count = KMS_USB_RULES + 1; CHECK(!kms_usb_policy_valid(&p));
    p = restored; p.rules[0].enabled = 2; CHECK(!kms_usb_policy_valid(&p));
    p = restored; p.reserved[1] = 1; CHECK(!kms_usb_policy_valid(&p));
    kms_usb_policy_reset(&p, true); CHECK(p.count == 0 && kms_usb_policy_enabled(&p, &a));

    static const uint8_t keyboard[] = {5,1,9,6,0xa1,1,5,7,0x19,0,0x29,0x65,0x15,0,0x25,0x65,0x75,8,0x95,1,0x81,0,0xc0};
    static const uint8_t mouse[] = {5,1,9,2,0xa1,1,9,0x30,9,0x31,0x15,0x81,0x25,0x7f,0x75,8,0x95,2,0x81,6,0xc0};
    static const uint8_t media[] = {5,0xc,9,1,0xa1,1,0x19,0,0x2a,0xff,3,0x15,0,0x26,0xff,3,0x75,16,0x95,1,0x81,0,0xc0};
    static kms_hid_parser_t parser;
    CHECK(kms_hid_parse(&parser, keyboard, sizeof(keyboard)));
    CHECK(kms_usb_hid_kinds(&parser) == KMS_USB_KEYBOARD);
    CHECK(kms_hid_parse(&parser, mouse, sizeof(mouse)));
    CHECK(kms_usb_hid_kinds(&parser) == KMS_USB_MOUSE);
    CHECK(kms_hid_parse(&parser, media, sizeof(media)));
    CHECK(kms_usb_hid_kinds(&parser) == KMS_USB_CONSUMER);
    memset(&parser, 0, sizeof(parser)); CHECK(kms_usb_hid_kinds(&parser) == 0);

    kms_usb_candidate_t combo[] = {
        {.device=1, .endpoints=1, .kinds=KMS_USB_KEYBOARD, .enabled=true},
        {.device=1, .endpoints=1, .kinds=KMS_USB_MOUSE, .enabled=true},
        {.device=2, .endpoints=1, .kinds=KMS_USB_KEYBOARD, .enabled=true},
        {.device=2, .endpoints=1, .kinds=KMS_USB_MOUSE, .enabled=true}};
    CHECK(kms_usb_plan(combo, 4, 4) == 15); // Both generic keyboard/mouse combos fit.
    combo[1].kinds = KMS_USB_CONSUMER;
    CHECK(kms_usb_plan(combo, 4, 6) == 12); // Keep the complete keyboard/mouse receiver; never leave its keyboard unpolled.
    CHECK(kms_usb_plan(combo, 4, 8) == 0);
    kms_usb_candidate_t three[] = {
        {.device=1, .endpoints=1, .kinds=KMS_USB_KEYBOARD, .enabled=true},
        {.device=2, .endpoints=1, .kinds=KMS_USB_KEYBOARD, .enabled=true},
        {.device=3, .endpoints=1, .kinds=KMS_USB_KEYBOARD, .enabled=true}};
    uint32_t selected = kms_usb_plan(three, 3, 2);
    CHECK(selected == 3 || selected == 5 || selected == 6); // Never activate a third physical input, even with spare channels.
    three[2].preferred = true;
    CHECK(kms_usb_plan(three, 3, 2) & 4);
    three[0].enabled = three[1].enabled = false;
    CHECK(kms_usb_plan(three, 3, 2) == 4);
    kms_usb_candidate_t output_only = {.device=1, .endpoints=1, .kinds=KMS_USB_LEDS, .enabled=true};
    CHECK(kms_usb_plan(&output_only, 1, 2) == 0);
    combo[2].enabled = false;
    CHECK(kms_usb_plan(combo, 4, 6) == 3); // Blocking one interface excludes the whole physical bundle.
    uint32_t seed = 0x315b890u;
    for (unsigned run = 0; run < 1000; ++run) {
        kms_usb_candidate_t c[8];
        for (unsigned n = 0; n < 8; ++n) {
            seed = seed * 1664525u + 1013904223u;
            c[n] = (kms_usb_candidate_t){.device=(seed >> 3) % 4 + 1, .endpoints=(seed >> 9) % 3 + 1,
                .kinds=(uint8_t)(1u << ((seed >> 16) % 4)), .enabled=(seed & 1) != 0,
                .preferred=(seed & 16) != 0, .active=(seed & 32) != 0};
        }
        unsigned baseline = seed % 9, cost = baseline, device_bits = 0;
        selected = kms_usb_plan(c, 8, baseline);
        for (unsigned n = 0; n < 8; ++n) if (selected & (1u << n)) {
            CHECK(c[n].enabled); cost += c[n].endpoints; device_bits |= 1u << c[n].device;
            for (unsigned j = 0; j < 8; ++j) if (c[j].device == c[n].device)
                CHECK(selected & (1u << j)); // No partial physical input device in any varied case.
        }
        unsigned devices_count = 0;
        for (unsigned n = 0; n < 8; ++n) if (device_bits & (1u << n)) ++devices_count;
        CHECK(cost <= KMS_USB_CHANNELS);
        CHECK(devices_count <= KMS_USB_ACTIVE_DEVICES);
    }
    return 0;
}
