# Stress regression coverage

The stress tests cover workload planning, USB status parsing, startup retries,
generator leases, cancellation, delivery faults, checkpoint verification and recovery.
Fixtures use synthetic identities and deterministic inputs.

Run the stress suite from the repository root.

~~~powershell
python scripts/ci-tests.py stress
~~~

The tests exercise the production runner with fake channels and capture adapters.
For device validation, use the [Stress Toolkit guide](stress-testing.md).
