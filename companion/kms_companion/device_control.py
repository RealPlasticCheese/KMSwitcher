"""Bounded configuration commands through the helper's single HID owner."""
from .protocol import Op, words, read_words
from .transport import DeviceError, RejectedCommand

USB_STATES = ('probing', 'active', 'off', 'channel-limit', 'unsupported', 'error', 'removing', 'ignored', 'standby')
JIGGLER_MODES = ('Left/right', 'Four-way', 'Random strokes')


def request(receiver, operation, data=b'', *, unsupported=None):
    try:
        return receiver.request(operation, data, timeout=.6)
    except RejectedCommand as error:
        if str(error) == 'unsupported firmware':
            raise DeviceError(unsupported or 'Update the controller and attached dongle in Firmware Setup to use device management.') from error
        raise
    except DeviceError as error:
        raise DeviceError('Device command was not acknowledged. Check the controller connection and update both firmware roles if needed. Refresh before retrying a change.') from error


def execute(receiver, command, args):
    if command == 'device-remote-start':
        slot, operation = args['slot'], args['operation']
        if type(slot) is not int or not 1 <= slot <= 7 or operation not in (1, 2, 3, 4, 5, 21):
            raise ValueError('Invalid diagnostic request')
        payload = bytes.fromhex(args.get('data', ''))
        if len(payload) != (4 if operation == 4 else 0):
            raise ValueError('Invalid diagnostic payload')
        data = bytes((slot, operation, len(payload), 0)) + words(args.get('offset', 0), args.get('boot', 0)) + payload
        return dict(token=read_words(request(receiver, Op.REMOTE_REQUEST, data).data, 1)[0])
    if command == 'device-remote-read':
        try:
            data = request(receiver, Op.REMOTE_READ, words(args['token'], args.get('offset', 0))).data
        except RejectedCommand as error:
            if str(error) == 'busy': return dict(pending=True)
            raise
        status, length, boot, offset = read_words(data, 4)
        if status >= 12 or length > 192 or offset != args.get('offset', 0) or offset > length:
            raise ValueError('Invalid remote diagnostic response')
        return dict(pending=False, status=status, length=length, boot=boot, offset=offset,
                    data=data[16:16 + min(32, length - offset)].hex())
    if command.startswith('device-usb-'):
        return usb_request(receiver, command, args)
    actions = {'device-status': 0, 'device-pair': 1, 'device-unpair': 2,
               'device-pause': 3, 'device-resume': 4, 'device-jiggler-get': 5,
               'device-jiggler-set': 6, 'device-jiggler-mode': 7}
    if command not in actions: raise ValueError('Unknown device command')
    action, slot, value = actions[command], args.get('slot', 0), args.get('value', 0)
    if type(slot) is not int or not 0 <= slot <= 7 or (action in (1, 2, 5, 6, 7) and slot == 0):
        raise ValueError('Select a dongle from 1 to 7')
    if type(value) is not int or (action == 6 and value not in (0, 1)) or (action == 7 and value not in (0, 1, 2)):
        raise ValueError('Invalid jiggler setting')
    data = request(receiver, Op.DEVICE_CONTROL, words(action, slot, value),
                   unsupported='Update the selected dongle in Firmware Setup to use its mouse jiggler controls.' if action >= 5 else None).data
    if action < 5:
        paired, selected, pairing = read_words(data, 3)
        if paired > 127 or selected > 7 or pairing > 1: raise ValueError('Invalid device status')
        return dict(paired=[i for i in range(1, 8) if paired & (1 << (i - 1))], selected=selected, pairing=bool(pairing))
    boot, sequence, seconds, distance, mode, enabled, pending, supported = read_words(data, 8)
    if not boot or not 1 <= seconds <= 65535 or not 1 <= distance <= 127 or mode > 2 or max(enabled, pending, supported) > 1:
        raise ValueError('Invalid jiggler status')
    return dict(boot=boot, sequence=sequence, seconds=seconds, distance=distance,
                mode=mode, enabled=bool(enabled), pending=bool(pending), supported=bool(supported))


def usb_request(receiver, command, args):
    if command == 'device-usb-snapshot':
        token, devices, interfaces, controls, hubs, inputs, automatic, limit = read_words(request(receiver, Op.USB_SNAPSHOT).data, 8)
        if not token or devices > 8 or interfaces > 24 or automatic > 1 or limit != 8:
            raise ValueError('Invalid USB inventory')
        return dict(token=token, devices=devices, interfaces=interfaces, controls=controls,
                    hubs=hubs, inputs=inputs, automatic=bool(automatic), limit=limit)
    token, index = args['token'], args.get('index', 0)
    if command == 'device-usb-change':
        action, enabled = args['action'], args.get('enabled', 0)
        if action not in (0, 1, 2) or type(enabled) is not int or enabled not in (0, 1):
            raise ValueError('Invalid USB selection')
        request(receiver, Op.USB_CHANGE, words(token, action, args.get('address', 0), args.get('number', 0), enabled))
        return dict(applied=True)
    operation = {'device-usb-device': Op.USB_DEVICE, 'device-usb-interface': Op.USB_INTERFACE}.get(command)
    if operation is None or type(index) is not int or not 0 <= index < (8 if operation == Op.USB_DEVICE else 24):
        raise ValueError('Invalid USB inventory index')
    data = request(receiver, operation, words(token, index)).data
    if read_words(data, 2) != (token, index): raise ValueError('USB inventory changed; refresh')
    if operation == Op.USB_DEVICE:
        vidpid, serial = read_words(data, 2, 8)
        address, parent, port, speed = data[16:20]
        return dict(address=address, vid=f'{vidpid & 65535:04x}', pid=f'{vidpid >> 16:04x}', serial=serial,
                    parent=parent, port=port, speed=speed, name=data[20:].split(b'\0', 1)[0].decode('utf-8', errors='replace'))
    _, _, address, number, device_class, endpoints, kinds, wanted, state, descriptor_bytes, reports, decoded = read_words(data, 12)
    if not 1 <= address <= 127 or number > 255 or device_class > 255 or endpoints > 255 or kinds > 31 or wanted > 1 or state >= len(USB_STATES):
        raise ValueError('Invalid USB interface')
    return dict(address=address, number=number, device_class=device_class, endpoints=endpoints, kinds=kinds,
                wanted=bool(wanted), state=USB_STATES[state], descriptor_bytes=descriptor_bytes, reports=reports, decoded=decoded)
