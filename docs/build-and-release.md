# Build KMSwitcher

GitHub Actions builds and tests Windows and Linux installers, firmware bundles,
and the standalone Engineering Console. Its KMSwitcher CI check requires every
build and test job to pass.

## Firmware on Windows

Install Python, Git and the pinned SDK with the [development setup](../README.md#development-tools).
Build receiver firmware before the controller so the controller can include its matching image.

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/idf.ps1 -Project receiver -Release -ResetConfig
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/idf.ps1 -Project controller -Release -ResetConfig
python scripts/release-version.py --component firmware
python scripts/package-firmware.py --build build/production --output build/release/firmware
~~~

Keep the application images, manifests, partition tables and bootloaders together.
The package command validates the matching roles, rollback configuration and cached receiver firmware.
See [flashing and pairing](flashing-and-pairing.md) before using a build with hardware.

## Engineering Console

Node.js versions supported by web/package.json are required.

~~~powershell
cd web
npm ci
npm test
npm run build
~~~

The standalone console is written to web/dist/KMSwitcher.html.

## Python applications

Create a virtual environment and install the platform's locked dependencies from the repository root.

~~~powershell
python -m venv .venv
.venv\Scripts\python.exe scripts/install-build-dependencies.py --platform windows
.venv\Scripts\python.exe scripts/release-version.py --component companion
.venv\Scripts\python.exe scripts/build-companion.py --product companion
~~~

Use the setup component and product for Firmware Setup.
Stress Toolkit packaging runs on Linux with the linux dependency lock and stress component and product.
The applications and firmware are independently versioned.
The frozen application is written under build/release/app.

## Validation

See [testing](testing.md) for local checks.
Before distributing generated installers or firmware, review the actual bundled dependency licenses and notices.
Source license information is in [LICENSE](../LICENSE) and [third-party notices](../THIRD-PARTY-NOTICES.md).

## Publish a GitHub release

Run the Build and test workflow on main with a release tag such as v0.2.0.

~~~powershell
gh workflow run build.yml --ref main -f release_tag=v0.2.0
~~~

A blank release tag runs validation without publishing. The workflow uses the
component versions in versions/, verifies every asset against its build
manifest, and publishes the verified draft on [GitHub Releases](https://github.com/RealPlasticCheese/KMSwitcher/releases).
Each release includes SHA256SUMS.txt and the index used by Firmware Setup and the
Stress Toolkit. Windows packages are built without signing credentials.

The Azure pipeline templates are retained for reference.
