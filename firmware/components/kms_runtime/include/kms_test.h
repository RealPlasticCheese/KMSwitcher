#pragma once
#include "kms_core.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

enum { KMS_TEST_FEATURE = 126, KMS_TEST_PAGE = 128, KMS_TEST_RECORD_BYTES = 92,
       KMS_TEST_RECORDS = 256, KMS_TEST_RESTART = 7, KMS_TEST_CAN_RESTART = 16 };
#define KMS_TEST_RESTART_KEY 0x52535431u
enum kms_test_stage { KMS_T_RAW = 1, KMS_T_DECODED, KMS_T_ROUTED, KMS_T_TX,
    KMS_T_RX, KMS_T_ACCEPTED, KMS_T_REJECTED, KMS_T_USB_QUEUE,
    KMS_T_USB_SUBMIT, KMS_T_USB_COMPLETE, KMS_T_CLEAR, KMS_T_FAULT, KMS_T_SELECT, KMS_T_INJECT,
    KMS_T_RADIO_SEND_CALL, KMS_T_LINK_HANDLED };
#ifdef CONFIG_KMS_TEST_SUPPORT
void kms_test_init(uint32_t boot, bool master);
void kms_test_record(unsigned stage, uint32_t a, uint32_t b, uint32_t c,
                     const void *data, size_t bytes);
void kms_test_sample(unsigned stage, uint32_t a, const kms_sample_t *sample);
bool kms_test_console(const char *line);
bool kms_test_drop(unsigned packet_type);
bool kms_test_usb_available(void);
bool kms_test_take_restart(void);
void kms_test_command(const uint8_t *data, size_t size);
size_t kms_test_page(uint8_t *out, size_t capacity);
#else
static inline void kms_test_init(uint32_t boot, bool master) { (void)boot; (void)master; }
#define kms_test_record(...) ((void)0)
#define kms_test_sample(...) ((void)0)
#define kms_test_drop(...) false
static inline bool kms_test_usb_available(void) { return true; }
static inline bool kms_test_take_restart(void) { return false; }
static inline bool kms_test_console(const char *line) { (void)line; return false; }
#endif
