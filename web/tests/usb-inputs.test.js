import test from 'node:test';
import assert from 'node:assert/strict';
import { SerialConnection } from '../serial.js';
import { DemoPort } from '../demo.js';
import { usbInventory, usbCommand, usbKinds } from '../usb-inputs.js';

test('USB inventory lists controls, hub costs and separate HID functions; choices work through serial', async t => {
  const port = new DemoPort(), connection = new SerialConnection();
  await connection.open(port); t.after(() => connection.close());
  let inventory = await usbInventory(connection);
  assert.equal(inventory.devices.length, 3);
  assert.equal(inventory.used, 8);
  assert.equal(inventory.devices[1].name, 'Numeric keypad');
  assert.equal(inventory.interfaces.filter(row => row.deviceClass === 3).length, 4);
  assert.equal(usbKinds(3), 'Keyboard + Mouse');
  await usbCommand(connection, 'usb-auto off');
  inventory = await usbInventory(connection);
  assert.equal(inventory.automatic, false);
  assert.equal(inventory.inputs, 4); // Manual selection retains working input.
  await usbCommand(connection, 'usb-input 2 1 off');
  inventory = await usbInventory(connection);
  assert.equal(inventory.inputs, 2);
  assert.ok(inventory.interfaces.filter(row => row.address === 2).every(row => !row.wanted));
  await usbCommand(connection, 'usb-input 3 0 off');
  await usbCommand(connection, 'usb-input 2 0 on');
  await usbCommand(connection, 'usb-input 3 1 on');
  inventory = await usbInventory(connection);
  assert.equal(inventory.used, 8);
  assert.deepEqual(inventory.interfaces.filter(row => row.wanted).map(row => [row.address, row.number]), [[2, 0], [2, 1], [3, 0], [3, 1]]);
  await assert.rejects(usbCommand(connection, 'usb-input 99 0 on'), /rejected/);
  await assert.rejects(usbInventory({ exchange: async (_, accept) => accept('KMSUSB BEGIN 1 3 1 2 9') }), /header/);
});

test('malformed or stale USB inventories cannot present misleading selectable devices', async () => {
  const read = lines => usbInventory({ exchange: async (_, accept) => {
    let result; for (const line of lines) { const value = accept(line); if (value !== undefined) result = value; }
    return result;
  } });
  const begin = 'KMSUSB BEGIN 1 2 1 1 8';
  const device = 'KMSUSB DEVICE 2 1241 1203 00000000 1 1 0 706164';
  const row = 'KMSUSB INTERFACE 2 0 03 1 1 1 active 62 10 10';
  await assert.rejects(read([begin, row, 'KMSUSB END']), /changed/);
  await assert.rejects(read([begin, device, device]), /device/);
  await assert.rejects(read([begin, device, row, row]), /interface/);
  await assert.rejects(read([begin, device, row.replace('active', 'nonsense')]), /interface/);
  await assert.rejects(read([begin, device, row.replace('10 10', '4294967296 10')]), /interface/);
  await assert.rejects(read([begin, device.replace('706164', '<img>')]), /device/);
  assert.throws(() => usbCommand({}, 'usb-input 2 0 on\nselect 1'), /Invalid/);
});
