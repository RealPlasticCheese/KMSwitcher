# Third-party notices

KMSwitcher's original source and artwork are available under the [MIT License](LICENSE).

- The standalone Engineering Console bundles fflate 0.8.2. Its [MIT license and copyright notice](web/fflate-LICENSE.txt) are included in the generated HTML.
- Firmware builds use [ESP-IDF](https://github.com/espressif/esp-idf/blob/master/docs/en/COPYRIGHT.rst) and the components named in the firmware and diagnostics lockfiles, including Espressif USB components and [TinyUSB](https://github.com/hathach/tinyusb/blob/master/LICENSE). Diagnostic builds can generate overlays of upstream SDK source. The upstream licenses apply to that source and the relevant portions of compiled firmware.
- Frozen desktop distributions generate a third-party inventory and copy notices from the build environment. They can contain Python, Qt/PySide6, esptool, PyInstaller's bootloader, and additional native libraries and Python packages. See the installation's third-party-notices folder and the upstream terms for the specific binaries.
- The Windows installer is built with [Inno Setup](https://jrsoftware.org/isinfo.php), which is downloaded separately.
