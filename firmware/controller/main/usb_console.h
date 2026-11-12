#pragma once
#include <stdbool.h>
void kms_usb_console_init(void);
void kms_usb_console_list(void);
bool kms_usb_console_command(const char *line);
// Called only by the USB-owner task, after main has queued a command.
bool kms_usb_console_execute(const char *line);
// Freeze metadata before recovery cancels transfers; output runs on the console worker.
struct kms_usb_fault_snapshot;
bool kms_usb_console_fault_queue(const struct kms_usb_fault_snapshot *snapshot);
