#include "kms_radio.h"
#include <string.h>
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
static unsigned checks;
__declspec(dllexport) int kms_run_radio_tests(void)
{
    checks = 0;
    CHECK(kms_radio_channel_valid(6, 1, 13));
    CHECK(kms_radio_channel_valid(13, 1, 13));
    CHECK(!kms_radio_channel_valid(0, 1, 13));
    CHECK(!kms_radio_channel_valid(12, 1, 11));
    CHECK(!kms_radio_channel_valid(1, 0, 13));
    CHECK(!kms_radio_channel_valid(1, 1, 14));
    kms_radio_message_t input = {.type = KMS_RADIO_QUERY, .channel = 6, .slot = 7,
        .boot = 0x12345678, .nonce = 0x98765432}, output;
    uint8_t wire[KMS_RADIO_BYTES];
    CHECK(kms_radio_encode(&input, wire));
    CHECK(kms_radio_decode(&output, wire, sizeof(wire)));
    CHECK(output.nonce == input.nonce && output.boot == input.boot && output.slot == 7 && output.channel == 6);
    CHECK(!kms_radio_decode(&output, wire, sizeof(wire) - 1));
    wire[4] = 2; CHECK(!kms_radio_decode(&output, wire, sizeof(wire))); wire[4] = 1;
    wire[18] = 1; CHECK(!kms_radio_decode(&output, wire, sizeof(wire))); wire[18] = 0;
    wire[7] = 0; CHECK(!kms_radio_decode(&output, wire, sizeof(wire))); wire[7] = 7;
    input.type = KMS_RADIO_REPLY; input.channel = 11; input.delay_ms = 1800;
    CHECK(kms_radio_encode(&input, wire)); CHECK(kms_radio_decode(&output, wire, sizeof(wire)));
    CHECK(output.delay_ms == 1800 && output.channel == 11);
    input.delay_ms = 1801; CHECK(!kms_radio_encode(&input, wire));
    input.delay_ms = 1; input.type = KMS_RADIO_QUERY; CHECK(!kms_radio_encode(&input, wire));
    input.delay_ms = 0; input.nonce = 0; CHECK(!kms_radio_encode(&input, wire));
    input.type = KMS_RADIO_PROBE; input.nonce = 73; input.delay_ms = 0;
    CHECK(kms_radio_encode(&input, wire) && kms_radio_decode(&output, wire, sizeof(wire)));
    CHECK(output.type == KMS_RADIO_PROBE && output.nonce == 73 && !output.flags && !output.delay_ms);
    input.flags = KMS_RADIO_PROBE_BUSY; CHECK(!kms_radio_encode(&input, wire));
    input.type = KMS_RADIO_PROBE_REPLY; input.delay_ms = KMS_RADIO_PROBE_TIMEOUT_MS - 1;
    CHECK(kms_radio_encode(&input, wire) && kms_radio_decode(&output, wire, sizeof(wire)));
    CHECK(output.flags == KMS_RADIO_PROBE_BUSY && output.delay_ms == input.delay_ms);
    ++input.delay_ms; CHECK(!kms_radio_encode(&input, wire));
    --input.delay_ms; input.flags = 2; CHECK(!kms_radio_encode(&input, wire));
    wire[19] = 1; CHECK(!kms_radio_decode(&output, wire, sizeof(wire)));
    kms_radio_search_t search;
    kms_radio_search_init(&search, 6, 0);
    CHECK(kms_radio_search_tick(&search, 3999, 1, 13, false, true, false) == 6);
    CHECK(!search.searching);
    CHECK(kms_radio_search_tick(&search, 4000, 1, 13, false, true, false) == 7);
    CHECK(search.searching);
    CHECK(kms_radio_search_tick(&search, 4399, 1, 13, false, true, false) == 7);
    CHECK(kms_radio_search_tick(&search, 4400, 1, 13, false, true, true) == 7); // OTA/candidate freeze
    CHECK(kms_radio_search_tick(&search, 4400, 1, 13, false, true, false) == 8);
    kms_radio_search_contact(&search, 4400);
    CHECK(!search.searching);
    CHECK(kms_radio_search_tick(&search, 8399, 1, 13, false, true, false) == 8);
    // Offline slave finds a master moved to channel 1 without re-pairing.
    bool found = false;
    for (uint32_t now = 8400; now < 14000; now += 400) {
        if (kms_radio_search_tick(&search, now, 1, 13, false, true, false) == 1) {
            kms_radio_search_contact(&search, now); found = true; break;
        }
    }
    CHECK(found && !search.searching && search.channel == 1);
    kms_radio_search_init(&search, 6, 0);
    CHECK(kms_radio_search_tick(&search, 10000, 1, 13, false, false, false) == 6); // unpaired idle stays quiet
    CHECK(kms_radio_search_tick(&search, 10000, 1, 13, true, false, false) == 7); // physical pairing scans
    CHECK(kms_radio_search_tick(&search, 11000, 1, 13, true, false, true) == 7);
    kms_radio_search_init(&search, 11, 0xfffffff0u);
    CHECK(kms_radio_search_tick(&search, 0x1000, 1, 11, false, true, false) == 1); // clock wrap + country limit
    return 0;
}
__declspec(dllexport) int kms_radio_test_count(void) { return (int)checks; }
