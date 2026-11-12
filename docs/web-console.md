# Engineering Console

The [standalone console](../web) provides a live terminal and local firmware
build tools. Use [Companion](installing.md#optional-companion) for pairing,
mouse jigglers, USB inputs, radio settings and remote dongle diagnostics. Use
[standalone Firmware Setup](installing.md#standalone-firmware-setup) to flash,
update or restart devices. Both desktop apps can open the same console file
through **Open Engineering Console**.

## Open and connect

Open the downloaded `console/KMSwitcher.html` in desktop Chrome or Edge with
Web Serial enabled. It works from disk without a server or internet connection.
Use **Locate Engineering Console** in either desktop app to remember a moved
file or select a browser other than your default.

To build the self-contained HTML from source, run `.\web\package-html.cmd`,
or `npm --prefix web ci` followed by `npm --prefix web run build`, using a
supported Node.js version from [package.json](../web/package.json). Open
`web/dist/KMSwitcher.html`. The unbundled source page loads JavaScript modules
and needs HTTPS or a loopback server (for example `node web/serve.js`).

1. Connect the S3’s **COM/UART** socket. Close Firmware Setup’s serial operation
   and any other serial monitors before connecting; only one program can own
   the port.
2. Select **Console speed** and **Connect controller**, then choose its port in
   the browser picker. The console never selects or opens a port on its own.
3. The connection starts at 115200 baud and uses no hardware flow control. It
   leaves the serial reset signals untouched. With compatible controller
   firmware it can negotiate higher speeds up to 4,000,000 baud.
4. Use **Disconnect** before opening Firmware Setup on the same port.
   Disconnect restores 115200. If a browser tab closes or stops responding,
   the controller returns to 115200 when its speed lease expires (within 10
   seconds); reconnect after that if needed.

## Terminal and diagnostic commands

Type a firmware command or use the **help**, **about**, **status**,
**history**, **slaves**, and **Save checkpoint** shortcuts. Up/Down recall
commands. **Download log** saves the captured serial output, including
protocol frames hidden from the terminal by default. Use **Show transfer
frames** to display them and **Clear view** to clear the local capture.
The console retains up to 10,000 lines or four million characters and shows
the latest 700 lines. Save any evidence before closing or reloading the tab.

**Maximise** fills the browser viewport; Escape or **Restore** returns to the
regular layout without interrupting the connection. The **Theme** menu
supports light, dark and system appearance. **Explore demo** simulates
terminal output without touching hardware.

See [recovery and diagnostics](recovery-and-diagnostics.md) for advanced
commands and [slave management](slave-management.md) for the UART command-line
tools. The [Companion Diagnostics tab](installing.md#device-management-in-companion)
can inspect and archive individual dongles without a separate UART cable.

## Build firmware

The build panel generates a Windows launcher or PowerShell command for the
repository’s pinned ESP-IDF setup. Choose the checkout path and target, then
download and run the launcher or copy the command. **Regenerate SDK
configuration** discards the generated configuration in favour of the
repository defaults. The browser cannot run local scripts or tell when a
build finishes.

The repository also supplies `web/build-all.cmd`, `web/build-controller.cmd`
and `web/build-receivers.cmd`. Build output stays in the checkout. After a
successful build, use **Firmware Setup → Choose local application** to select
the application `.bin` with its matching `.json` manifest beside it. That
flow is for OTA-capable devices. Use a verified firmware bundle and direct USB
for initial installation, partition migration or wired recovery.

## Validation

`npm --prefix web test` checks the file-based console, terminal controls and
build launcher, alongside protocol regression fixtures. `npm --prefix web run
build` produces the portable HTML with all resources inline and a restrictive
content security policy.
