#include "kms_clipboard.h"
#include <string.h>
static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
__declspec(dllexport) int kms_clipboard_sim_codec(const uint8_t *data, uint8_t *out)
{ kms_clipboard_packet_t p; return kms_clipboard_decode(&p, data, KMS_C_BYTES) && kms_clipboard_encode(&p, out); }
__declspec(dllexport) int kms_run_clipboard_tests(void)
{
    checks = 0;
    kms_clipboard_packet_t p = {.kind = KMS_C_DATA_CHUNK, .source = 1, .target = 2, .length = KMS_C_DATA,
        .source_session = 101, .target_session = 202, .sequence = 3, .transfer = 4, .offset = 160};
    for (unsigned i = 0; i < KMS_C_DATA; ++i) p.data[i] = (uint8_t)i;
    uint8_t wire[KMS_C_BYTES]; kms_clipboard_packet_t decoded;
    CHECK(kms_clipboard_encode(&p, wire)); CHECK(kms_clipboard_decode(&decoded, wire, sizeof(wire)));
    CHECK(!memcmp(decoded.data, p.data, KMS_C_DATA) && decoded.offset == 160 && decoded.transfer == 4);
    for (unsigned i = 0; i < sizeof(wire); ++i) {
        wire[i] ^= 1; CHECK(!kms_clipboard_decode(&decoded, wire, sizeof(wire))); wire[i] ^= 1;
    }
    CHECK(!kms_clipboard_decode(&decoded, wire, sizeof(wire) - 1));
    static kms_desktop_master_t m; memset(&m, 0, sizeof(m));
    m.peers[0] = (kms_desktop_peer_t){.session = 101, .boot = 11, .seen = 1000};
    m.peers[1] = (kms_desktop_peer_t){.session = 202, .boot = 22, .seen = 1000};
    CHECK(kms_clipboard_authorized(&m, &p, 1, 11, 1001));
    CHECK(!kms_clipboard_authorized(&m, &p, 1, 12, 1001));
    CHECK(!kms_clipboard_authorized(&m, &p, 2, 11, 1001));
    CHECK(!kms_clipboard_authorized(&m, &p, 1, 11, 4000));
    m.peers[1].session = 203; CHECK(!kms_clipboard_authorized(&m, &p, 1, 11, 1001));
    p.target = 1; CHECK(!kms_clipboard_encode(&p, wire));
    return 0;
}
__declspec(dllexport) int kms_clipboard_test_count(void) { return (int)checks; }
