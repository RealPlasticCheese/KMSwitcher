# Production routing verification

Run `python scripts/audit-routing.py` from the repository root. It compiles the
production firmware router and tests real CROSS/PREPARE/READY/SELECT operations
without communicating with hardware. Production violations always fail the run.

`model.py` is an independent executable specification for differential checks.
Firmware uses `firmware/components/kms_core/desktop_routes.c`; neither firmware nor
the companion imports the Python reference. See the [algorithm, coverage and
limits](../../docs/routing-reachability-audit.md).
