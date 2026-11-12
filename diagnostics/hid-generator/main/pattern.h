#pragma once
#include <stdint.h>
enum { KMS_GEN_BYTES = 54, KMS_GEN_VID = 0x303a, KMS_GEN_PID = 0x4006, KMS_GEN_MIXED = 5 };
// modifiers, 256 key bits, buttons (u16), X/Y (i16), wheel/pan (i8), consumer
// (u16), "KM", source (u8), run/sequence (u32). Report ID is not included.
void kms_generator_pattern(uint8_t *, unsigned, uint32_t, uint32_t, uint32_t, unsigned);
