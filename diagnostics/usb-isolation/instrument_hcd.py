"""Generate an observation-only HCD overlay; never modify managed components."""
import hashlib
from pathlib import Path
import sys

UPSTREAM_SHA256 = "f7be5cff2be70176baab93e1b4e643e1d5bd3cd56383f01cc79392c76c5adc15"

def instrument(data):
    if hashlib.sha256(data).hexdigest() != UPSTREAM_SHA256:
        raise ValueError("USB Host 1.6.0 HCD source differs from the reviewed source")
    source = data.decode()
    def add(anchor, replacement):
        nonlocal source
        if source.count(anchor) != 1:
            raise ValueError("Ambiguous/missing HCD trace anchor: " + anchor)
        source = source.replace(anchor, replacement)
    add("// With s_port_inited[]", '#include "trace_hcd.inc"\n\n// With s_port_inited[]')
    add("    usb_dwc_hal_port_event_t hal_port_evt = usb_dwc_hal_decode_intr(port->hal);",
        "    iso_driver_isr();\n    usb_dwc_hal_port_event_t hal_port_evt = usb_dwc_hal_decode_intr(port->hal);")
    add("    usb_dwc_hal_chan_event_t chan_event = usb_dwc_hal_chan_decode_intr(chan_obj);",
        "    iso_trace_pipe(ISO_TRACE_IRQ, pipe, pipe->buffers[pipe->multi_buffer_control.rd_idx]->urb, "
        "USB0.host_chan_regs[chan_obj->flags.chan_idx * 8 + 2]);\n"
        "    usb_dwc_hal_chan_event_t chan_event = usb_dwc_hal_chan_decode_intr(chan_obj);")
    add("    // Add the pipe to the list of idle pipes in the port object",
        "    iso_trace_pipes[pipe->chan_obj->flags.chan_idx] = pipe;\n"
        "    // Add the pipe to the list of idle pipes in the port object")
    add("    usb_dwc_hal_chan_free(pipe->port->hal, pipe->chan_obj);",
        "    iso_trace_pipes[pipe->chan_obj->flags.chan_idx] = NULL;\n"
        "    usb_dwc_hal_chan_free(pipe->port->hal, pipe->chan_obj);")
    activation = "    usb_dwc_hal_chan_activate(pipe->chan_obj, buffer_to_exec->xfer_desc_list, desc_list_len, start_idx);"
    add(activation, "    uint32_t iso_before = USB0.hfnum;\n" + activation + "\n    iso_trace_activation(pipe, buffer_to_exec->urb, desc_list_len, iso_before);")
    continuation = "    usb_dwc_hal_chan_activate(pipe->chan_obj, buffer_inflight->xfer_desc_list, XFER_LIST_LEN_CTRL, buffer_inflight->flags.ctrl.cur_stg);"
    add(continuation, "    uint32_t iso_before = USB0.hfnum;\n" + continuation + "\n    iso_trace_activation(pipe, buffer_inflight->urb, buffer_inflight->flags.ctrl.cur_stg, iso_before);")
    add("    urb_t *urb = buffer_to_parse->urb;",
        "    urb_t *urb = buffer_to_parse->urb;\n"
        "    iso_trace_pipe(ISO_TRACE_PARSED, pipe, urb, urb->transfer.status);")
    add("    pipe->num_urb_pending++;",
        "    pipe->num_urb_pending++;\n    iso_trace_pipe(ISO_TRACE_ENQUEUE, pipe, urb, submit_urb);")
    add("        pipe->num_urb_done--;",
        "        pipe->num_urb_done--;\n        iso_trace_pipe(ISO_TRACE_DEQUEUE, pipe, urb, urb->transfer.status);")
    add("    HCD_EXIT_CRITICAL_ISR();\n\n    if (yield) {", "    HCD_EXIT_CRITICAL_ISR();\n    iso_driver_isr_exit();\n\n    if (yield) {")
    return source

if __name__ == "__main__":
    result = instrument(Path(sys.argv[1]).read_bytes())
    Path(sys.argv[2]).write_text(result)
