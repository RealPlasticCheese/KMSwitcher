"""Production S2 evidence persistence and S3 automatic-restart worker checks."""
import ctypes
from pathlib import Path
import subprocess

from native_build import NativeCompiler

root = Path(__file__).resolve().parents[1]
out = root / 'build/receiver-recovery-tests'; out.mkdir(parents=True, exist_ok=True)
remote = (root / 'firmware/components/kms_runtime/remote.c').read_text()
sections = {
    'remote_types.inc': ('enum job_type', 'static QueueHandle_t'),
    'remote_send.inc': ('static bool send_frame(', 'static uint32_t next_id('),
    'remote_take.inc': ('bool kms_remote_take_outgoing(', 'typedef struct { uint32_t id, target, offset, token, at;'),
    'remote_budget.inc': ('static bool recovery_save(', 'static void observe_upload('),
    'remote_request.inc': ('static kms_management_frame_t make_request(', 'static void automatic_restart('),
    'remote_auto.inc': ('static void automatic_restart(', 'static void configure_recovery('),
    'remote_config.inc': ('static void configure_recovery(', 'static void list_peers('),
    'remote_restart_gate.inc': ('bool kms_remote_restart_pending(', 'static uint32_t boot_id'),
}
for name, (first, last) in sections.items():
    (out / name).write_text(remote[remote.index(first):remote.index(last)])
# Run the receiver's exact restart branch with the production evidence writer.
first = remote.index('        bool busy = kms_ota_busy() || !kms_ota_confirmed()', remote.index('static void slave_request('))
last = remote.index('    } else if (kms_remote_restart_pending()', first)
(out / 'slave_restart.inc').write_text('static void restart_request(const kms_management_frame_t *q, kms_management_frame_t *reply) {\n'
    '    kms_management_frame_t r = {0};\n' + remote[first:last] + '    *reply = r;\n}\n')
first = remote.index('        kms_desktop_message_t request;', remote.index('static void slave_worker('))
last = remote.index('        kms_management_frame_t q;', first)
submit = remote.index('bool kms_remote_local_diagnostics(')
(out / 'slave_local_jobs.inc').write_text('static void service_local_job(void) {\n' + remote[first:last] + '}\n' +
    remote[submit:remote.index('void kms_remote_init(', submit)])

compiler = NativeCompiler()
core = root / 'firmware/components/kms_core'
common = [core / name for name in ('management.c', 'slave_history.c', 'slave_snapshot.c', 'receiver_recovery.c', 'wire.c', 'jiggler.c')]
common += [root / 'tests/native/runtime.c']
sources = common + [root / ('tests/native/test_' + name + '.c') for name in ('slave_diagnostics', 'remote_recovery', 'jiggler_store')]
for source in sources:
    subprocess.run(compiler.compile_command(
        source, out / (source.stem + '.obj'),
        includes=[
            root / 'tests/native/recovery/include',
            root / 'tests/native/history-store/include',
            root / 'tests/native/include',
            core / 'include',
            root / 'config',
            out,
            root / 'firmware/components/kms_runtime/include'],
        disabled_warnings=[4244, 4267]), check=True)
for name in ('slave_diagnostics', 'remote_recovery', 'jiggler_store'):
    dll = out / (name + '.dll')
    objects = [str(out / (source.stem + '.obj')) for source in common]
    subprocess.run(compiler.link_command(dll, [str(out / ('test_' + name + '.obj')), *objects]), check=True)
    library = ctypes.CDLL(str(dll))
    failure = getattr(library, 'kms_run_' + name + '_tests')()
    if failure:
        line = (root / 'tests/native' / ('test_' + name + '.c')).read_text().splitlines()[failure - 1]
        raise SystemExit(f'FAIL test_{name}.c:{failure}: {line.strip()}')
    print(f'PASS: {getattr(library, "kms_" + name + "_test_count")()} production {name} checks')
