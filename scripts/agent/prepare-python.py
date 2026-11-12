"""Reuse a hash-locked Python environment on a persistent Linux build agent."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import venv

ROOT = Path(__file__).resolve().parents[2]


def prepare():
    lock = ROOT / 'packaging/requirements-linux.lock'
    fingerprint = hashlib.sha256(lock.read_bytes() + sys.version.encode()).hexdigest()[:20]
    cache = Path(os.environ.get('AGENT_WORKFOLDER', Path.home() / 'kms-build-cache')) / 'kms-cache/python'
    environment = cache / fingerprint
    python = environment / 'bin/python'
    marker = environment / '.installed'
    if not python.exists():
        venv.EnvBuilder(with_pip=True).create(environment)
    if not marker.exists() or subprocess.run([str(python), '-m', 'pip', 'check'], capture_output=True).returncode:
        subprocess.run([str(python), str(ROOT / 'scripts/install-build-dependencies.py')], check=True)
        marker.touch()
    print('##vso[task.prependpath]' + str(python.parent))
    print('Using persistent Python environment: ' + str(environment))


if __name__ == '__main__':
    prepare()
