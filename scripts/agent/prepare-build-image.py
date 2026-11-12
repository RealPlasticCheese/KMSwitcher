"""Build/reuse the Linux packaging image from a minimal dependency-only context."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
FILES = ('packaging/linux/Dockerfile.build', 'packaging/requirements-linux.lock',
         'scripts/install-build-dependencies.py', 'scripts/package-linux.py')


def prepare():
    digest = hashlib.sha256()
    for name in FILES:
        digest.update(name.encode())
        digest.update((ROOT / name).read_bytes())
    image = 'kmswitcher-linux-build:' + digest.hexdigest()[:20]
    if subprocess.run(['docker', 'image', 'inspect', image], capture_output=True).returncode:
        builder = 'kmswitcher-builder'
        if subprocess.run(['docker', 'buildx', 'inspect', builder], capture_output=True).returncode:
            subprocess.run(['docker', 'buildx', 'create', '--name', builder, '--driver', 'docker-container',
                            '--driver-opt', 'memory=6g,cpu-quota=200000,cpu-period=100000'], check=True)
        with tempfile.TemporaryDirectory(prefix='kms-build-image-') as directory:
            context = Path(directory)
            for name in FILES:
                target = context / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / name, target)
            subprocess.run(['docker', 'buildx', 'build', '--builder', builder, '--load', '-f', str(context / FILES[0]),
                            '-t', image, str(context)], check=True,
                           env=dict(os.environ, DOCKER_BUILDKIT='1'))
    print('##vso[task.setvariable variable=linuxBuildImage]' + image)
    print('Linux packaging image: ' + image)


if __name__ == '__main__':
    prepare()
