"""Guard the actual CHENA write in the reviewed ESP-IDF 6.1 S3 HAL copy."""
import argparse
import hashlib
from pathlib import Path

SHA256 = "e0b24c9e1f9753ca70301d18cc69b114f79f939f680dfcfd0a96fbb75e5966f5"
ANCHOR = "    usb_dwc_ll_hcchar_enable_chan(chan_obj->regs); // Start the channel\n    chan_obj->flags.active = 1;"


def instrument(data, trace=False):
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError("Unreviewed USB HAL source")
    source = data.decode()
    if source.count(ANCHOR) != 1 or source.count("#include <stddef.h>") != 1:
        raise ValueError("Ambiguous HAL activation anchor")
    header, prefix = ("frame_guard.h", "iso_guard") if trace else ("kms_usb_frame_guard.h", "kms_usb_frame_guard")
    includes = f'#include <stddef.h>\n#include "{header}"\n#include "soc/usb_struct.h"'
    if trace:
        includes += '\n#include "driver_trace.h"'
    source = source.replace("#include <stddef.h>", includes)
    replacement = ("    bool kms_guard_interrupt_in = chan_obj->type == USB_DWC_XFER_TYPE_INTR && chan_obj->regs->hcchar_reg.epdir;\n"
        f"    {prefix}_before_enable(kms_guard_interrupt_in);\n"
        "    uint32_t kms_guard_before = USB0.hfnum;\n"
        "    usb_dwc_ll_hcchar_enable_chan(chan_obj->regs); // Start the channel\n"
        "    uint32_t kms_guard_after = USB0.hfnum;\n"
        "    chan_obj->flags.active = 1;\n")
    if trace:
        replacement += "    iso_driver_enable_edge(chan_obj->flags.chan_idx, kms_guard_before, kms_guard_after);\n"
    replacement += f"    {prefix}_after_enable(kms_guard_interrupt_in, kms_guard_before, kms_guard_after);"
    return source.replace(ANCHOR, replacement)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--trace", action="store_true")
    args = parser.parse_args()
    args.output.write_text(instrument(args.source.read_bytes(), args.trace), encoding="utf-8")
