"""Pinned TinyUSB device-side observation overlay for the synthetic generators."""
import hashlib
from pathlib import Path
import sys
SHA256 = "27678dabda2c227c1d472d6cd6b77770287c399cf4944f061e594561ef9be4b3"
def instrument(data):
    if hashlib.sha256(data).hexdigest() != SHA256: raise ValueError("Unreviewed TinyUSB DCD source")
    source = data.decode()
    changes = [
        ('#include "tusb_option.h"', '#include "tusb_option.h"\n#include "generator_trace.h"'),
        ('    edpt_schedule_packets(rhport, epnum, dir);\n    ret = true;',
         '    edpt_schedule_packets(rhport, epnum, dir);\n    if (ep_addr == 0x81) kms_gen_trace_arm(buffer,total_bytes,xfer->max_size);\n    ret = true;'),
        ('  const uint32_t gintsts = dwc2->gintsts & gintmask;',
         '  const uint32_t gintsts = dwc2->gintsts & gintmask;\n  kms_gen_trace_irq_enter(gintsts);'),
        ('}\n\n#if CFG_TUD_TEST_MODE', '  kms_gen_trace_irq_exit();\n}\n\n#if CFG_TUD_TEST_MODE')]
    for old,new in changes:
        if old.startswith("    edpt_schedule_packets"):
            start = source.index("bool dcd_edpt_xfer(")
            end = source.index("bool dcd_edpt_xfer_fifo(",start)
            chunk = source[start:end]
            if chunk.count(old) != 1: raise ValueError("Ambiguous endpoint-arm anchor")
            source = source[:start] + chunk.replace(old,new) + source[end:]
            continue
        if source.count(old) != 1: raise ValueError("Ambiguous DCD trace anchor: " + repr(old))
        source = source.replace(old,new)
    return source
if __name__ == "__main__": Path(sys.argv[2]).write_text(instrument(Path(sys.argv[1]).read_bytes()))
