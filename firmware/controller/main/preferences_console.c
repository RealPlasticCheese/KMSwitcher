#include "preferences.h"
#include "kms_management.h"
#include "kms_remote.h"
#include "kms_jiggler.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void reply(unsigned long id, const char *status)
{
    uint8_t data[KMS_PREFERENCES_BYTES]; char hex[KMS_PREFERENCES_BYTES * 2 + 1];
    kms_user_preferences_encode(kms_preferences_get(), data); kms_hex_encode(data, sizeof(data), hex);
    printf("KMSPREFS %lu %s %" PRIu32 " %s\n", id, status, kms_preferences_revision(), hex);
}
bool kms_preferences_console(const char *line)
{
    bool distance = !strncmp(line, "jiggler-distance", 16) && (!line[16] || line[16] == ' ');
    bool interval = !strncmp(line, "jiggler-interval", 16) && (!line[16] || line[16] == ' ');
    if (distance || interval) {
        if (!line[16]) {
            printf("KMSJIGGLER %s %u\n", distance ? "DISTANCE" : "INTERVAL",
                distance ? kms_preferences_get()->jiggler_distance : kms_preferences_get()->jiggler_seconds); return true;
        }
        unsigned value = 0, maximum = distance ? KMS_JIGGLER_MAX_DISTANCE : KMS_JIGGLER_MAX_SECONDS;
        const char *text = line + 17;
        bool valid = *text != 0;
        while (valid && *text) {
            if (*text < '0' || *text > '9') valid = false;
            else {
                value = value * 10 + (unsigned)(*text - '0');
                valid = value <= maximum;
            }
            ++text;
        }
        valid = valid && value >= 1;
        kms_user_preferences_t changed = *kms_preferences_get();
        if (valid) {
            if (distance) changed.jiggler_distance = (uint8_t)value;
            else changed.jiggler_seconds = (uint16_t)value;
        }
        bool saved = valid && kms_preferences_set(&changed);
        printf("KMSJIGGLER %s %u\n", saved ? "SAVED" : "ERROR",
            distance ? kms_preferences_get()->jiggler_distance : kms_preferences_get()->jiggler_seconds);
        return true;
    }
    unsigned slot; int end = 0;
    if (!strcmp(line, "boot-default")) {
        printf("KMSBOOT %u (0=automatic)\n", kms_preferences_get()->boot_slot); return true;
    }
    if (sscanf(line, "boot-default %u%n", &slot, &end) == 1 && !line[end]) {
        bool saved = kms_preferences_save_boot_slot(NULL, slot);
        printf("KMSBOOT %s %u\n", saved ? "SAVED" : "ERROR", kms_preferences_get()->boot_slot);
        return true;
    }
    unsigned long id, expected; int consumed = 0;
    if (sscanf(line, "prefs-get %lu%n", &id, &consumed) == 1 && !line[consumed]) {
        reply(id, "OK"); return true;
    }
    bool reset = sscanf(line, "prefs-default %lu %lu%n", &id, &expected, &consumed) == 2 && !line[consumed];
    if (!reset && !(sscanf(line, "prefs-set %lu %lu %n", &id, &expected, &consumed) == 2 && consumed > 0)) return false;
    if (kms_remote_master_updating()) { reply(id, "BUSY"); return true; }
    if (expected != kms_preferences_revision()) { reply(id, "CONFLICT"); return true; }
    kms_user_preferences_t changed;
    if (reset) kms_user_preferences_defaults(&changed);
    else {
        uint8_t data[KMS_PREFERENCES_BYTES]; size_t bytes;
        if (!kms_hex_decode(line + consumed, data, sizeof(data), &bytes) ||
            !kms_user_preferences_decode(&changed, data, bytes)) { reply(id, "INVALID"); return true; }
    }
    reply(id, kms_preferences_set(&changed) ? "OK" : "STORAGE"); return true;
}
