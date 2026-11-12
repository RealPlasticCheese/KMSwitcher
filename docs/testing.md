# Testing KMSwitcher

Tests use deterministic adapters, synthetic fixtures and native production-code harnesses.
Hardware acceptance requires a separate rig and explicit device selection.

## Local checks

Install the locked dependencies as described in [building](build-and-release.md).
Run from the repository root with the virtual environment's Python.

~~~powershell
python scripts/ci-tests.py native
python scripts/ci-tests.py sdk
python scripts/ci-tests.py python
cd web
npm ci
npm test
npm run build
~~~

The native suites need the Visual Studio C++ build tools on Windows or GCC on Linux.
The SDK suites need the pinned ESP-IDF and USB component sources.
Qt tests use the offscreen backend.
Linux desktop backends and device access also need validation on Linux.

## Hardware acceptance

Review [flashing and pairing](flashing-and-pairing.md), then verify both firmware roles,
keyboard and mouse forwarding, lock LEDs, switching, reconnects and firmware updates.
Use [Stress Toolkit](stress-testing.md) for repeatable workloads and local evidence.
Store lab reports, logs and diagnostic exports outside the public source checkout.
