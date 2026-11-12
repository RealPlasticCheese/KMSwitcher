#include "stream.h"
#include "pattern.h"
#include <string.h>
static unsigned checks;
#define CHECK(c) do { ++checks; if (!(c)) return __LINE__; } while (0)
__declspec(dllexport) int kms_run_isolation_stream_tests(void)
{
    checks = 0;
    const uint8_t config[] = {9,2,34,0,1,1,0,0x80,50, 9,4,0,0,1,3,0,0,0,
                             9,0x21,0x11,1,0,1,0x22,100,0, 7,5,0x81,3,64,0,1};
    iso_endpoint_t endpoint;
    CHECK(iso_find_endpoint(config, sizeof(config), &endpoint));
    CHECK(endpoint.interface == 0 && endpoint.endpoint == 0x81 && endpoint.packet == 64);
    for (size_t n = 0; n < sizeof(config); ++n) CHECK(!iso_find_endpoint(config, n, &endpoint));
    uint8_t invalid[sizeof(config)];
    for (unsigned n = 0; n < sizeof(config); ++n) {
        if (n != 0 && n != 9 && n != 18 && n != 27) continue;
        memcpy(invalid, config, sizeof(config)); invalid[n] = 0;
        CHECK(!iso_find_endpoint(invalid, sizeof(invalid), &endpoint));
    }
    memcpy(invalid, config, sizeof(config)); invalid[29] = 1;
    CHECK(!iso_find_endpoint(invalid, sizeof(invalid), &endpoint));
    memcpy(invalid, config, sizeof(config)); invalid[31] = 8;
    CHECK(!iso_find_endpoint(invalid, sizeof(invalid), &endpoint));
    uint8_t serial[25] = {0}; char identity[13];
    const char *text = "020000001002";
    for (unsigned n = 0; n < 12; ++n) serial[1 + n * 2] = (uint8_t)text[n];
    CHECK(iso_serial(serial + 1, 24, identity));
    CHECK(!memcmp(identity, "020000001002", 13));
    CHECK(!iso_serial(serial + 1, 23, identity));
    serial[2] = 1; CHECK(!iso_serial(serial + 1, 24, identity));
    CHECK(iso_elapsed(0x10, 0xfffffff0u, 32));
    CHECK(!iso_elapsed(0x0f, 0xfffffff0u, 32));
    for (unsigned source = 1; source <= 2; ++source) for (unsigned profile = 0; profile <= 4; ++profile) {
        iso_stream_t stream = {.run = 123, .seed = 456, .profile = profile};
        uint8_t report[55] = {1};
        for (unsigned n = 1; n <= 100; ++n) {
            kms_generator_pattern(report + 1, source, 123, n, 456, profile);
            iso_stream_report(&stream, report, sizeof(report));
        }
        CHECK(stream.source == source && stream.count == 100 && !stream.sequence_errors && !stream.payload_errors);
        uint32_t crc = stream.crc;
        kms_generator_pattern(report + 1, source, 123, 0, 456, profile);
        iso_stream_report(&stream, report, sizeof(report));
        CHECK(stream.releases == 1 && stream.count == 100 && stream.crc == crc);
        kms_generator_pattern(report + 1, source, 122, 9, 456, profile);
        iso_stream_report(&stream, report, sizeof(report)); CHECK(stream.stale == 1 && stream.crc == crc);
        kms_generator_pattern(report + 1, source, 123, 102, 456, profile);
        iso_stream_report(&stream, report, sizeof(report)); CHECK(stream.sequence_errors == 1);
        report[3] ^= 1; iso_stream_report(&stream, report, sizeof(report)); CHECK(stream.payload_errors == 1);
        iso_stream_report(&stream, report, sizeof(report) - 1); CHECK(stream.payload_errors == 2);
        report[46] = 3; iso_stream_report(&stream, report, sizeof(report)); CHECK(stream.payload_errors == 3);
    }
    return 0;
}
__declspec(dllexport) unsigned kms_isolation_stream_check_count(void) { return checks; }
