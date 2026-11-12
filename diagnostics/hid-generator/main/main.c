#include "pattern.h"
#ifdef KMS_GENERATOR_TRACE
#include "generator_trace.h"
#define GENERATOR_TRACE_ID " trace=1"
#else
#define GENERATOR_TRACE_ID ""
#endif
#include "kms_management.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

// All functions share one IN endpoint. The vendor metadata adds no endpoint.
static const uint8_t descriptor[] = {
    5,1,9,6,0xa1,1,0x85,1,
    5,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
    0x19,0,0x2a,0xff,0,0x75,1,0x96,0,1,0x81,2,
    5,8,0x19,1,0x29,5,0x75,1,0x95,5,0x91,2,0x75,3,0x95,1,0x91,1,0xc0,
    5,1,9,2,0xa1,1,0x85,1,9,1,0xa1,0,
    5,9,0x19,1,0x29,16,0x15,0,0x25,1,0x75,1,0x95,16,0x81,2,
    5,1,9,0x30,9,0x31,0x16,1,0x80,0x26,0xff,0x7f,0x75,16,0x95,2,0x81,6,
    9,0x38,0x15,0x81,0x25,0x7f,0x75,8,0x95,1,0x81,6,
    5,0x0c,0x0a,0x38,2,0x81,6,0xc0,0xc0,
    5,0x0c,9,1,0xa1,1,0x85,1,0x15,0,0x27,0xff,0xff,0,0,
    0x19,0,0x2a,0xff,0xff,0x75,16,0x95,1,0x81,0,0xc0,
    6,0,0xff,9,1,0xa1,1,0x85,1,0x15,0,0x26,0xff,0,
    0x75,8,0x95,11,9,1,0x81,2,0xc0
};
static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = KMS_GEN_VID, .idProduct = KMS_GEN_PID, .bcdDevice = 0x0100,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1
};
static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof(descriptor), 0x81, 64, 1)
};
static char identity[13];
static const char *strings[] = {"\x09\x04", "KMSwitcher", "KMSwitcher Test Generator", identity, "Synthetic input"};
static uint32_t run, seed, source = 1, rate = 20, seconds = 10, profile;
static uint32_t offered, submitted, completed, missed, digest, last_ping;
static uint32_t led_updates;
static uint8_t leds;
static uint64_t started, next_due;
static uint8_t payload[KMS_GEN_BYTES];
static bool running, inflight, releasing, release_inflight, detached;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static bool usb_busy(void)
{
    portENTER_CRITICAL(&lock); bool result = inflight || detached; portEXIT_CRITICAL(&lock); return result;
}
static void usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    // A USB bus reset clears TinyUSB's endpoints without an unmount callback.
    // ATTACHED follows SetConfiguration, when old transfers are also invalid.
    // Retire their ownership and stop the run; do not count them as delivered.
    if (event->id == TINYUSB_EVENT_DETACHED || event->id == TINYUSB_EVENT_ATTACHED) {
        portENTER_CRITICAL(&lock); inflight = false; detached = true; portEXIT_CRITICAL(&lock);
    }
}
const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) { (void)instance; return descriptor; }
uint16_t tud_hid_get_report_cb(uint8_t i, uint8_t id, hid_report_type_t t, uint8_t *out, uint16_t n)
{ (void)i; (void)id; (void)t; (void)out; (void)n; return 0; }
void tud_hid_set_report_cb(uint8_t i, uint8_t id, hid_report_type_t t, const uint8_t *data, uint16_t n)
{
    if (i == 0 && id == 1 && t == HID_REPORT_TYPE_OUTPUT && n == 1) {
        portENTER_CRITICAL(&lock); leds = data[0] & 31u; ++led_updates; portEXIT_CRITICAL(&lock);
    }
}
void tud_hid_report_complete_cb(uint8_t i, const uint8_t *report, uint16_t n)
{
    (void)i; (void)report; (void)n;
#ifdef KMS_GENERATOR_TRACE
    kms_gen_trace_mark(3,n);
#endif
    portENTER_CRITICAL(&lock);
    if (inflight) {
        if (!release_inflight) { ++completed; digest = kms_crc32_extend(digest, payload, sizeof(payload)); }
        inflight = false;
    }
    portEXIT_CRITICAL(&lock);
}
static void stop(void) { running = false; releasing = true; }
static void stats(void)
{
    portENTER_CRITICAL(&lock);
    uint32_t done = completed, crc = digest; bool pending = inflight || detached;
    portEXIT_CRITICAL(&lock);
    printf("KMSGEN STATS %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %" PRIu32 " %08" PRIx32 " %u %u\n",
           run, source, offered, submitted, done, missed, crc, (unsigned)running, (unsigned)(pending || releasing));
}
static bool configure(unsigned r, unsigned s, unsigned sd, unsigned hz, unsigned duration, unsigned mode)
{
    if (running || releasing || usb_busy() || !r || s < 1 || s > 2 ||
        !hz || hz > 1000 || !duration || duration > 86400 || mode > KMS_GEN_MIXED) return false;
    run = r; source = s; seed = sd; rate = hz; seconds = duration; profile = mode;
    return true;
}
static void command(const char *line)
{
#ifdef KMS_GENERATOR_TRACE
    unsigned token; int consumed=0;
    if (sscanf(line,"bench trace freeze %u%n",&token,&consumed)==1 && token && !line[consumed]) {
        kms_gen_trace_freeze(); printf("KMSGEN TRACEFROZEN id=%u\n",token); return;
    }
    if (sscanf(line,"bench trace dump %u%n",&token,&consumed)==1 && token && !line[consumed]) {
        kms_gen_trace_print(token,identity); return;
    }
#endif
    unsigned r, s, sd, hz, duration, mode; int used = 0;
    if (!strcmp(line, "bench info")) printf("KMSGEN INFO 1 %s %u %s" GENERATOR_TRACE_ID "\n", identity, (unsigned)KMS_GEN_BYTES, esp_app_get_description()->version);
    else if (!strcmp(line, "bench ping")) last_ping = (uint32_t)(esp_timer_get_time() / 1000);
    else if (!strcmp(line, "bench stats")) stats();
    else if (!strcmp(line, "bench leds")) {
        portENTER_CRITICAL(&lock); unsigned mask = leds; uint32_t updates = led_updates; portEXIT_CRITICAL(&lock);
        printf("KMSGEN LEDS %u %" PRIu32 "\n", mask, updates);
    }
    else if (!strcmp(line, "bench stop")) { stop(); printf("KMSGEN STOPPING\n"); }
    else if (!strcmp(line, "bench start")) {
        if (running || releasing || usb_busy() || !run || !tud_mounted() || tud_suspended()) {
            printf("KMSGEN ERROR not ready\n"); return;
        }
#ifdef KMS_GENERATOR_TRACE
        kms_gen_trace_start(run);
#endif
        offered = submitted = missed = 0;
        portENTER_CRITICAL(&lock); completed = digest = 0; portEXIT_CRITICAL(&lock);
        started = next_due = (uint64_t)esp_timer_get_time(); last_ping = (uint32_t)(started / 1000); running = true;
        printf("KMSGEN STARTED %" PRIu32 "\n", run);
    } else if (sscanf(line, "bench config %u %u %u %u %u %u%n", &r, &s, &sd, &hz, &duration, &mode, &used) == 6 && !line[used]) {
        if (!configure(r, s, sd, hz, duration, mode)) {
            printf("KMSGEN ERROR invalid configuration\n"); return;
        }
        printf("KMSGEN CONFIGURED %" PRIu32 " %" PRIu32 "\n", run, source);
    } else printf("KMSGEN ERROR invalid command\n");
}
static void commands(void)
{
    static char line[160]; static unsigned length; static bool overflow;
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\r' || c == '\n') {
            if (length || overflow) { line[length] = 0; if (!overflow) command(line); else printf("KMSGEN ERROR line too long\n"); }
            length = 0; overflow = false;
        } else if (length < sizeof(line) - 1) line[length++] = c;
        else overflow = true;
    }
}
static void poll(uint64_t now)
{
#ifdef KMS_GENERATOR_TRACE
    kms_gen_trace_tick();
#endif
    portENTER_CRITICAL(&lock); bool lost = detached; detached = false; portEXIT_CRITICAL(&lock);
    if (lost) stop();
    if (running && (now - started >= (uint64_t)seconds * 1000000u ||
                    (uint32_t)(now / 1000) - last_ping >= 2000 || !tud_mounted() || tud_suspended())) stop();
    portENTER_CRITICAL(&lock); bool busy = inflight; portEXIT_CRITICAL(&lock);
    if (!busy && releasing && tud_hid_n_ready(0)) {
        kms_generator_pattern(payload, source, run, 0, seed, profile);
        portENTER_CRITICAL(&lock); inflight = release_inflight = true; portEXIT_CRITICAL(&lock);
        if (tud_hid_n_report(0, 1, payload, sizeof(payload))) releasing = false;
        else { portENTER_CRITICAL(&lock); inflight = false; portEXIT_CRITICAL(&lock); }
    } else if (running && now >= next_due) {
        uint32_t interval = 1000000u / rate;
        uint32_t ticks = (uint32_t)((now - next_due) / interval) + 1;
        next_due += (uint64_t)ticks * interval; offered += ticks; missed += ticks;
        if (!busy && tud_hid_n_ready(0)) {
            kms_generator_pattern(payload, source, run, submitted + 1, seed, profile);
            portENTER_CRITICAL(&lock); inflight = true; release_inflight = false; portEXIT_CRITICAL(&lock);
            if (tud_hid_n_report(0, 1, payload, sizeof(payload))) {
                ++submitted; --missed;
#ifdef KMS_GENERATOR_TRACE
                kms_gen_trace_mark(2,submitted);
#endif
            }
            else { portENTER_CRITICAL(&lock); inflight = false; portEXIT_CRITICAL(&lock); }
        }
    }
}

void app_main(void)
{
    uint8_t mac[6]; ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(identity, sizeof(identity), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
    config.event_cb = usb_event;
    config.descriptor.device = &device; config.descriptor.full_speed_config = configuration;
    config.descriptor.string = strings; config.descriptor.string_count = sizeof(strings) / sizeof(strings[0]);
    ESP_ERROR_CHECK(tinyusb_driver_install(&config));
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    while (true) {
        poll((uint64_t)esp_timer_get_time());
        commands();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
