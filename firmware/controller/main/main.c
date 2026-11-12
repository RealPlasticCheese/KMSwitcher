#include "kms_runtime.h"
#include "diagnostic_log.h"
#include "kms_radio_channel.h"
#include "usb_input.h"
#include "usb_leds.h"
#include "usb_recovery.h"
#include "esp_random.h"
#include "usb_console.h"
#include "usb_fault.h"
#include "preferences.h"
#include "kms_diagnostic_store.h"
#include "console_info.h"
#include "console_baud.h"
#include "kms_remote.h"
#include "kms_ota.h"
#include "kms_test.h"
#include "kms_test_mode.h"
#include "kms_desktop_runtime.h"
#include "device_controls.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_system.h"
#include "key_trace.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static kms_router_t router;
static bool desktop_preferences(bool write, uint8_t data[KMS_PREFERENCES_BYTES], uint32_t *revision)
{
    if (write) {
        kms_user_preferences_t p;
        if (*revision != kms_preferences_revision() || !kms_user_preferences_decode(&p, data, KMS_PREFERENCES_BYTES) ||
            !kms_preferences_set(&p)) return false;
    }
    *revision = kms_preferences_revision();
    return kms_user_preferences_encode(kms_preferences_get(), data);
}
static void route_input(void *ctx, const kms_state_t *state, int32_t x, int32_t y, int32_t wheel, int32_t pan)
{
    if (!router.diagnostic_typing) kms_link_emit(ctx, state, x, y, wheel, pan);
}
static void apply_preferences(void)
{
    static uint32_t applied_revision;
    if (applied_revision == kms_preferences_revision()) return;
    const kms_user_preferences_t *p = kms_preferences_get();
    kms_router_set_preferences(&router, p); kms_board_set_preferences(p);
    kms_usb_input_leds_enabled(p->keyboard_leds != 0);
    kms_link_set_jiggler_settings(p->jiggler_seconds, p->jiggler_distance);
    kms_state_t zero = {0}; kms_link_emit(NULL, &zero, 0, 0, 0, 0);
    applied_revision = kms_preferences_revision();
}
static void status(void)
{
    ESP_LOGI("console", "Double-tap key: %s; configuration %s", kms_trigger_name(router.trigger),
             (router.mode == KMS_MODE_CONFIG || router.mode == KMS_MODE_DEFAULT) ? "open" : "closed");
    ESP_LOGI("console", "Boot dongle: %u (0=automatic)", kms_preferences_get()->boot_slot);
    ESP_LOGI("console", "Test mode: %s", kms_test_mode_active() ? "on (retained across restarts)" : "off");
    kms_link_status(); kms_usb_input_status(); kms_diagnostics_status();
}
static void restore_trigger(void)
{
    if (!kms_preferences_save_trigger(NULL, KMS_TRIGGER_DEFAULT)) { kms_board_signal(KMS_NOTICE_ERROR); return; }
    kms_router_set_trigger(&router, KMS_TRIGGER_DEFAULT);
    kms_state_t zero = {0}; kms_link_emit(NULL, &zero, 0, 0, 0, 0);
    kms_board_signal(KMS_NOTICE_RESTORED);
    ESP_LOGI("console", "Default trigger restored; computer pairings retained");
}
static void execute_command(const char *line)
{
    unsigned slot;
    if (!strcmp(line, "help")) kms_console_help();
    else if (kms_console_baud_command(line)) { /* UART worker owns speed changes. */ }
    else if (!strcmp(line, "about")) kms_console_about();
    else if (!strcmp(line, "status")) status();
    else if (!strncmp(line, "jiggler ", 8) || !strncmp(line, "jiggler-mode ", 13)) {
        unsigned long id = 0; unsigned target = 0; int end = 0; char value[16] = {0};
        kms_jiggler_status_t jiggle = {0}; const char *error = "INVALID";
        bool get = sscanf(line, "jiggler get %lu %u%n", &id, &target, &end) == 2 && !line[end];
        bool set = !get && sscanf(line, "jiggler set %lu %u %3s%n", &id, &target, value, &end) == 3 &&
            !line[end] && (!strcmp(value, "on") || !strcmp(value, "off"));
        bool mode = sscanf(line, "jiggler mode %lu %u %15s%n", &id, &target, value, &end) == 3 && !line[end];
        bool short_get = sscanf(line, "jiggler-mode %u%n", &target, &end) == 1 && !line[end];
        bool short_mode = !short_get && sscanf(line, "jiggler-mode %u %15s%n", &target, value, &end) == 2 && !line[end];
        if (get || set || mode || short_get || short_mode) {
            if (mode || short_mode) {
                unsigned choice = 0;
                while (choice < KMS_JIGGLER_MODE_COUNT && strcmp(value, kms_jiggler_mode_name(choice))) ++choice;
                error = kms_link_jiggler_set_mode(target, choice);
            } else error = set ? kms_link_jiggler_set(target, !strcmp(value, "on")) : NULL;
            if (!error) error = kms_link_jiggler_get(target, &jiggle);
        }
        if ((short_get || short_mode) && !error)
            ESP_LOGI("console", "Dongle %u jiggle mode: %s%s", target, kms_jiggler_mode_name(jiggle.mode),
                jiggle.pending ? " (awaiting confirmation)" : "");
        printf("KMSJIGGLER %lu %s %u %u %u %u %lu %lu %u %u %u\n", id, error ? error : "OK", target,
            jiggle.enabled, jiggle.seconds, jiggle.pending, (unsigned long)jiggle.boot, (unsigned long)jiggle.sequence,
            jiggle.distance, jiggle.mode, jiggle.modes_supported);
    }
    else if (!strncmp(line, "channel", 7) && (!line[7] || line[7] == ' ')) {
        unsigned long id = 0; unsigned channel; int end = 0;
        if (!strcmp(line, "channel")) kms_radio_channel_status(0, "OK");
        else if (sscanf(line, "channel get %lu%n", &id, &end) == 1 && !line[end]) kms_radio_channel_status(id, "OK");
        else if (sscanf(line, "channel set %lu %u%n", &id, &channel, &end) == 2 && !line[end]) {
            const char *error = kms_radio_channel_set(channel);
            kms_radio_channel_status(id, error ? error : "OK");
            if (!error) kms_router_block_held(&router);
        } else kms_radio_channel_status(id, "INVALID");
    }
    else if (!strcmp(line, "test-mode on") && kms_radio_channel_changing())
        printf("KMSTEST ERROR radio channel change in progress\n");
    else if (kms_controller_key_trace_console(line)) { /* Export on the trace worker. */ }
    else if (kms_test_console(line)) { /* Opt-in test trace control. */ }
    else if (kms_usb_console_command(line)) { /* USB inventory and selection. */ }
    else if (kms_diagnostic_log_console(line)) { /* Export on the log worker. */ }
    else if (kms_diagnostics_console(line)) { /* Optional age range; shared by both consoles. */ }
    else if (!strcmp(line, "stats-save")) kms_diagnostics_save();
    else if (!strcmp(line, "trigger-default")) restore_trigger();
    else if (!strcmp(line, "pause")) { kms_link_pause(); kms_router_block_held(&router); }
    else if (!strcmp(line, "resume")) { kms_link_resume(); kms_router_block_held(&router); }
    else if (kms_preferences_console(line)) { /* Validated and committed before applying. */ }
    else if (kms_remote_console(line)) { /* Queued on the management worker. */ }
    else if (!strncmp(line, "unpair", 6) && (!line[6] || line[6] == ' ')) {
        int end = 0;
        const char *error = "INVALID";
        slot = 0;
        if (sscanf(line, "unpair %u%n", &slot, &end) == 1 && !line[end]) {
            error = kms_link_unpair(slot);
            if (!error) kms_router_block_held(&router);
        }
        printf("KMSUNPAIR %u %s\n", slot, error ? error : "OK");
    }
    else if (sscanf(line, "pair %u", &slot) == 1 && slot >= 1 && slot <= KMS_SLOTS) {
        kms_link_pair(NULL, slot); kms_router_block_held(&router);
    }
    else if (sscanf(line, "select %u", &slot) == 1) {
        if (kms_link_select(NULL, slot)) {
            kms_router_block_held(&router);
            printf("KMSROUTE %s %u\n", kms_link_selected() == slot ? "SELECTED" : "PENDING", slot);
        } else printf("KMSROUTE ERROR selection unavailable\n");
    }
    else if (*line) ESP_LOGI("console", "Command not recognized or unavailable. Type help for available commands.");
}
static void commands(void)
{
    static char line[KMS_COMMAND_BYTES]; static unsigned length; static bool overflow;
    char console_line[KMS_COMMAND_BYTES];
    if (kms_diagnostic_console_command(console_line, sizeof(console_line))) execute_command(console_line);
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            if (overflow) {
                ESP_LOGW("console", "Command too long; discarded"); length = 0; overflow = false; continue;
            }
            line[length] = 0; execute_command(line);
            length = 0;
        } else if ((c == '\b' || c == 127) && length) --length;
        else if (c >= ' ') {
            if (length + 1 < sizeof(line)) line[length++] = c;
            else overflow = true;
        }
    }
}
void app_main(void)
{
    // Redirect shared streams before USB/radio workers can write to them.
    kms_console_baud_init();
    kms_diagnostic_log_init();
    kms_board_init(); kms_link_init(true, NULL, NULL);
    kms_controller_key_trace_init();
    kms_link_set_key_trace(kms_controller_key_trace);
    kms_test_init(esp_random(), true);
    kms_preferences_init(esp_random());
    kms_desktop_preferences_handler(desktop_preferences);
    kms_device_controls_init();
    kms_router_init(&router, route_input, kms_link_select, kms_link_pair, NULL);
    router.trace = kms_controller_router_key_trace;
    router.configure = kms_preferences_save_trigger;
    router.save_boot_slot = kms_preferences_save_boot_slot;
    router.toggle_jiggler = kms_link_toggle_jiggler;
    router.cycle_jiggler_mode = kms_link_cycle_jiggler_mode;
    router.dump_logs = kms_diagnostic_log_type;
    router.stop_logs = kms_diagnostic_log_stop;
    router.console_input = kms_diagnostic_console_input;
    kms_router_set_trigger(&router, kms_preferences_trigger());
    kms_usb_fault_init();
    kms_usb_input_init(&router); apply_preferences();
    kms_link_set_boot_slot(kms_usb_recovery_boot_slot(kms_preferences_get()->boot_slot));
    if (!kms_ota_confirmed() || kms_usb_recovery_boot_paused() || kms_test_mode_active()) kms_link_pause();
    kms_board_enable_controller_controls();
    kms_console_info_init();
    // Reopening fallback console streams can reset the shared UART input flags.
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    ESP_LOGI("kmswitcher", "Ready. Double-tap key: %s. Type help for commands or about for firmware information.", kms_trigger_name(router.trigger));
    while (true) {
        kms_desktop_block(router.mode != KMS_MODE_NORMAL || router.diagnostic_typing || kms_test_mode_active());
        kms_link_tick(true);
        if (kms_link_take_fault()) {
            if (router.diagnostic_typing) {
                kms_diagnostic_log_stop(NULL); router.diagnostic_typing = false;
            }
            kms_router_block_output(&router);
        }
        kms_usb_input_tick(); kms_device_controls_tick(); kms_router_tick(&router, kms_now()); commands(); apply_preferences();
        kms_link_controller_recovery_tick();
        if (kms_remote_master_updating()) kms_router_block_held(&router);
        kms_diagnostic_log_tick(&router);
        enum kms_notice notice = kms_router_take_notice(&router);
        if (notice != KMS_NOTICE_NONE) {
            kms_board_signal(notice);
            ESP_LOGI("console", "Shortcut configuration %s; current key: %s; boot dongle: %u (0=automatic)",
                     notice == KMS_NOTICE_SAVED ? "saved" : notice == KMS_NOTICE_CANCELLED ? "cancelled" : "failed",
                     kms_trigger_name(router.trigger), kms_preferences_get()->boot_slot);
        }
        enum kms_indicator led = kms_link_error() ? KMS_LED_ERROR : kms_link_pairing() ? KMS_LED_PAIR :
                                  (router.mode == KMS_MODE_CONFIG || router.mode == KMS_MODE_DEFAULT) ? KMS_LED_CONFIG : router.mode ? KMS_LED_SELECT : KMS_LED_NORMAL;
        kms_jiggler_status_t jiggle;
        if (led == KMS_LED_NORMAL && !kms_link_jiggler_get(kms_link_selected(), &jiggle) && jiggle.enabled)
            led = KMS_LED_JIGGLER;
        if (kms_board_tick(led, kms_link_selected())) status();
        enum kms_button_event button = kms_board_take_event();
        if (button == KMS_BUTTON_RESTORE_TRIGGER) restore_trigger();
        else if (button == KMS_BUTTON_FACTORY_RESET) {
            if (kms_preferences_factory_reset()) {
                ESP_LOGW("console", "Factory reset completed; restarting without paired computers");
                esp_restart(); // The reset gesture is emitted only after BOOT has been released.
            } else kms_board_signal(KMS_NOTICE_ERROR);
        }
        kms_app_wait();
    }
}
