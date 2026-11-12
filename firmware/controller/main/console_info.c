#include "console_info.h"
#include "kms_core.h"
#include "kms_radio_channel.h"
#include "kms_build.h"
#include <inttypes.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

enum info_request { SHOW_HELP, SHOW_ABOUT };
static QueueHandle_t requests;
static const char *TAG = "console";

static void print_help(void)
{
    ESP_LOGI(TAG, "  key-trace        Recent key path and dongle receipts; missing/unconfirmed keys log automatically");
    ESP_LOGI(TAG, "KMSwitcher master console - enter a command and press Enter:");
    ESP_LOGI(TAG, "  help             Show this command reference");
    ESP_LOGI(TAG, "  about            Show firmware and system information");
    ESP_LOGI(TAG, "  status           Show routing, USB devices, counters and recovery state");
    ESP_LOGI(TAG, "  usb-list         List USB identities, HID functions, endpoint costs and selection states");
    ESP_LOGI(TAG, "  usb-status [ID]  Show USB health; optional nonzero uint32 ID frames a correlated snapshot");
    ESP_LOGI(TAG, "  usb-fault        Print the latest USB fault snapshot captured before recovery");
    ESP_LOGI(TAG, "  usb-trace        Print current USB driver channel, interrupt and hub-status history");
    ESP_LOGI(TAG, "  usb-input A I on|off  Allow/block the whole input device at A/I; save by device identity");
    ESP_LOGI(TAG, "  usb-auto on|off  Clear overrides; automatic input / manual selection (keeps active inputs)");
    ESP_LOGI(TAG, "  usb-rescan       Retry descriptors on enumerated inactive HID interfaces");
    ESP_LOGI(TAG, "  usb-recover      Reconnect the USB host and rediscover inputs; retain the selected PC");
    ESP_LOGI(TAG, "  logs             Export buffered live diagnostic messages (up to one hour)");
    ESP_LOGI(TAG, "  During shortcut log typing, press I for Keyboard Console; Escape exits; US layout");
    ESP_LOGI(TAG, "  logs-status      Show PSRAM log capacity, age and loss counters");
    ESP_LOGI(TAG, "  history [X [Y]]  All retained history, last X minutes, or ages X..Y minutes (current boot)");
    ESP_LOGI(TAG, "  stats-save       Queue a statistics checkpoint to diagnostic storage");
    ESP_LOGI(TAG, "  trigger-default  Restore the default double-tap key (%s); retain pairings",
             kms_trigger_name(KMS_TRIGGER_DEFAULT));
    ESP_LOGI(TAG, "  pair N           Open enrollment for slot N (1-%u) for %u seconds",
             KMS_SLOTS, (unsigned)(KMS_PAIR_WINDOW_MS / 1000u));
    ESP_LOGI(TAG, "                   Routing pauses; hold the receiver button for %u seconds",
             (unsigned)(KMS_BUTTON_HOLD_MS / 1000u));
    ESP_LOGI(TAG, "  select N         Route input to an available paired computer (1-%u)", KMS_SLOTS);
    ESP_LOGI(TAG, "  unpair N         Remove slot N from this master only; receiver need not be online");
    ESP_LOGI(TAG, "  slaves           List paired dongles, contact times and cached firmware identities");
    ESP_LOGI(TAG, "  transfer-status  Show whether active firmware work blocks automatic handover");
    ESP_LOGI(TAG, "  receiver-recovery [on|off] Show/enable/disable bounded automatic dongle restart");
    ESP_LOGI(TAG, "  slave-status N   Fetch firmware identity, health, counters and retention (1-%u)", KMS_SLOTS);
    ESP_LOGI(TAG, "  slave-history N  Download and verify all retained history from slave N");
    ESP_LOGI(TAG, "  slave-history-clear N  Clear saved events/checkpoints; retain live counters and pairings");
    ESP_LOGI(TAG, "  pause / resume   Release input and pause routing / restore the previous route");
    ESP_LOGI(TAG, "  test-mode [on|off] Show/change test session; changes pause routing; on survives restarts");
    ESP_LOGI(TAG, "  bench arm N / freeze / off  Start, freeze or free a RAM trace within test mode");
    ESP_LOGI(TAG, "  Firmware/log files: manage-slaves.cmd --port COMn <operation>");
    ESP_LOGI(TAG, "  Operations: list | status --slot N | history --slot N --output FILE | clear --slot N");
    ESP_LOGI(TAG, "              update (--slot N | --all) (--firmware FILE | --cached) [--force]");
    ESP_LOGI(TAG, "              cache-status | cache-put --firmware FILE (store S2 firmware on S3)");
    ESP_LOGI(TAG, "  Updates skip identical image hashes; --force reinstalls with integrity checks enabled.");
    ESP_LOGI(TAG, "  boot-default [N] Show/save boot dongle (0=automatic, 1-7=wait for dongle); applies next boot");
    ESP_LOGI(TAG, "  jiggler-interval [SECONDS] Show/save mouse jiggler interval (1-65535, default 30)");
    ESP_LOGI(TAG, "  jiggler-distance [COUNTS] Show/save relative jiggle distance (1-127, default 1)");
    ESP_LOGI(TAG, "  jiggler get ID N / jiggler set ID N on|off  Query/set any paired dongle; ID correlates the reply");
    ESP_LOGI(TAG, "  jiggler-mode N [left-right|four-way|random] Query/select the mode of dongle N without enabling it");
    ESP_LOGI(TAG, "  Double-tap your configured trigger, then K to cycle the selected dongle's three jiggle modes");
    ESP_LOGI(TAG, "  Double-tap your configured trigger, then J to toggle the selected dongle's jiggler; it continues after switching away");
    ESP_LOGI(TAG, "  channel / channel get ID  Show current/pending radio channel and receiver support");
    ESP_LOGI(TAG, "  channel set ID N  Save/switch radio channel; upgrade all paired receivers first; routing stays paused");
    ESP_LOGI(TAG, "  prefs-get ID     Read saved everyday preferences and revision");
    ESP_LOGI(TAG, "  prefs-set ID REV HEX / prefs-default ID REV  Save/reset preferences; retain pairings");
    ESP_LOGI(TAG, "  baud get ID / baud set ID RATE  Query/negotiate UART speed (115200-4000000); 10s lease");
    ESP_LOGI(TAG, "  master HEX       Local master identity/OTA endpoint used by the browser");
    ESP_LOGI(TAG, "  remote N HEX     Framed transfer endpoint used by the PC helper");
}

static void print_about(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const kms_firmware_identity_t *identity = kms_build_identity();
    char image_sha256[65]; kms_hex_encode(identity->sha256, 32, image_sha256);
    char elf_sha256[65];
    esp_app_get_elf_sha256(elf_sha256, sizeof(elf_sha256));
    ESP_LOGI(TAG, "KMSwitcher - ESP32-S3 keyboard/mouse switcher master");
    ESP_LOGI(TAG, "Git: %s; application: %s", identity->git, app->project_name);
    ESP_LOGI(TAG, "Built UTC: %s; image bytes: %" PRIu32, identity->utc, identity->image_bytes);
    ESP_LOGI(TAG, "Firmware .bin SHA-256: %s", image_sha256);
    ESP_LOGI(TAG, "ESP-IDF: %s; input protocol: v%u; management protocol: v%u",
             app->idf_ver, KMS_WIRE_VERSION, KMS_M_VERSION);
    ESP_LOGI(TAG, "ELF SHA-256: %s", elf_sha256);
    ESP_LOGI(TAG, "Capacity: %u computer slots; ESP-NOW channel: %u (%s)",
             KMS_SLOTS, kms_radio_channel_get(), KMS_RADIO_COUNTRY);
    ESP_LOGI(TAG, "Type help for available commands; status for current operating state.");
}

static void info_worker(void *arg)
{
    (void)arg;
    enum info_request request;
    while (true) {
        if (xQueueReceive(requests, &request, portMAX_DELAY) != pdTRUE) continue;
        if (request == SHOW_HELP) print_help();
        else if (request == SHOW_ABOUT) print_about();
    }
}

void kms_console_info_init(void)
{
    requests = xQueueCreate(2, sizeof(enum info_request));
    if (!requests || xTaskCreate(info_worker, "kms-console", 3072, NULL, 1, NULL) != pdPASS) {
        if (requests) vQueueDelete(requests);
        requests = NULL;
        ESP_LOGW(TAG, "Console information worker unavailable; input continues");
    }
}

static void request_info(enum info_request request)
{
    if (!requests) ESP_LOGW(TAG, "Console help/about unavailable");
    else if (xQueueSend(requests, &request, 0) != pdTRUE)
        ESP_LOGW(TAG, "Console information busy; retry help/about shortly");
}

void kms_console_help(void) { request_info(SHOW_HELP); }
void kms_console_about(void) { request_info(SHOW_ABOUT); }
