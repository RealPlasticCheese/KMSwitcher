#pragma once
// Fixed by wire format v2 and the USB HID descriptors, not tuning settings.
enum {
    KMS_KEY_BYTES = 32, KMS_CONSUMERS = 4,
    KMS_WIRE_VERSION = 2, KMS_WIRE_SIZE = 137,
    // Seven minimum-size inputs fit a legacy 250-byte frame. Dense held states
    // reduce the transmitted count; receivers advertise their supported limit.
    KMS_INPUT_BATCH_MAX = 7, KMS_BATCH_VERSION = 2,
    KMS_BATCH_HEADER_BYTES = 24, KMS_BATCH_INPUT_TAIL_BYTES = 28,
    KMS_MAX_WIRE_SIZE = 250,
    KMS_FLAG_USB_READY = 1, KMS_FLAG_ACTIVE = 2,
    KMS_FLAG_JIGGLER_ENABLED = 4, KMS_FLAG_JIGGLER_SUPPORTED = 8, KMS_FLAG_JIGGLER_DISTANCE = 16,
    KMS_FLAG_JIGGLER_MODES = 32, KMS_JIGGLER_MODE_SHIFT = 6, KMS_JIGGLER_MODE_MASK = 0xc0,
    // STATUS public_key[6] follows the six-byte batch capability advertisement.
    KMS_STATUS_USB_SUSPENDED = 1,
    KMS_LOCK_NUM = 1, KMS_LOCK_CAPS = 2, KMS_LOCK_SCROLL = 4,
    KMS_LOCK_COMPOSE = 8, KMS_LOCK_KANA = 16, KMS_LOCK_MASK = 31,
    KMS_LOCK_VALID = 0x80,
    KMS_HID_ERROR_ROLLOVER = 1,
    KMS_HID_KEY_1 = 0x1e, KMS_HID_KEY_7 = 0x24,
    KMS_HID_KEYPAD_1 = 0x59, KMS_HID_KEYPAD_7 = 0x5f,
    KMS_HID_KEY_ESCAPE = 0x29, KMS_HID_KEY_P = 0x13, KMS_HID_KEY_C = 0x06, KMS_HID_KEY_D = 0x07,
    KMS_HID_KEY_J = 0x0d, KMS_HID_KEY_K = 0x0e, KMS_HID_KEY_L = 0x0f,
    KMS_HID_KEY_SCROLL_LOCK = 0x47, KMS_HID_KEY_INSERT = 0x49,
    KMS_HID_KEY_PAUSE = 0x48, KMS_HID_KEY_NUM_LOCK = 0x53,
    KMS_HID_CTRL_LEFT = 0x01, KMS_HID_CTRL_RIGHT = 0x10,
    KMS_HID_SHIFT_LEFT = 0x02, KMS_HID_SHIFT_RIGHT = 0x20,
    KMS_HID_ALT_LEFT = 0x04, KMS_HID_ALT_RIGHT = 0x40
};
