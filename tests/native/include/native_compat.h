#pragma once
// Preserve the test fixtures' export, noreturn and DMA-alignment declarations
// when compiling their existing MSVC spelling with the native Linux compiler.
#ifndef _MSC_VER
#define __declspec(value) KMS_NATIVE_DECLSPEC_##value
#define KMS_NATIVE_DECLSPEC_dllexport __attribute__((visibility("default")))
#define KMS_NATIVE_DECLSPEC_noreturn __attribute__((noreturn))
#define KMS_NATIVE_DECLSPEC_align(bytes) __attribute__((aligned(bytes)))
#endif
