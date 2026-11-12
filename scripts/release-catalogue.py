"""Assemble downloads from the latest packaged component builds or specific run IDs."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import tempfile
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = {'firmware': ['firmware'], 'companion': ['windows-companion', 'linux-companion'],
             'setup': ['windows-setup', 'linux-setup'], 'web': ['console'], 'stress': ['linux-stress'], 'stress-firmware': ['stress-firmware']}
REQUIRED_COMPONENTS = ('firmware', 'companion', 'setup', 'web')


def verify(directory, component, run_id):
    directory = Path(directory)
    metadata = json.loads((directory / 'component.json').read_text())
    if metadata['component'] != component or str(metadata['build_id']) != str(run_id):
        raise ValueError('Artifact component/run mismatch')
    listed = set()
    for entry in metadata['files']:
        path = (directory / entry['path']).resolve()
        if not path.is_relative_to(directory.resolve()) or path in listed:
            raise ValueError('Unsafe or duplicate artifact path')
        listed.add(path)
        if path.stat().st_size != entry['size'] or hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
            raise ValueError('Artifact checksum mismatch: ' + entry['path'])
    actual = {p.resolve() for p in directory.rglob('*') if p.is_file() and p.name != 'component.json'}
    if not listed or listed != actual:
        raise ValueError('Artifact inventory is incomplete')
    return metadata


def artifact_origin(url):
    address = urllib.parse.urlsplit(url)
    if address.scheme != 'https' or not address.hostname or address.username is not None or address.password is not None:
        raise ValueError('Artifact download must use HTTPS without URL credentials')
    return address.hostname, address.port or 443


def artifact_request(url):
    host, port = artifact_origin(url)
    collection = artifact_origin(os.environ['SYSTEM_COLLECTIONURI'])
    headers = {}
    # Pipeline Artifact content is served by a separate Azure artifact service,
    # not by the collection host. Signed storage URLs must not receive the token.
    if (host, port) == collection or (port == 443 and host.endswith('.artifacts.visualstudio.com')):
        headers['Authorization'] = 'Bearer ' + os.environ['SYSTEM_ACCESSTOKEN']
    return urllib.request.Request(url, headers=headers)


class ArtifactRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, *args, **kwargs):
        redirected = super().redirect_request(request, *args, **kwargs)
        if redirected is not None and artifact_origin(redirected.full_url) != artifact_origin(request.full_url):
            redirected.remove_header('Authorization')
        return redirected


def download_archive(request, archive, artifact):
    opener = urllib.request.build_opener(ArtifactRedirectHandler())
    with opener.open(request, timeout=120) as response:
        signature = response.read(4)
        if signature not in (b'PK\x03\x04', b'PK\x05\x06', b'PK\x07\x08'):
            content_type = response.headers.get('Content-Type', 'unknown')
            raise ValueError(f'{artifact}: artifact service did not return a ZIP archive '
                             f'(Content-Type: {content_type}). Check artifact download authentication.')
        with archive.open('wb') as destination:
            destination.write(signature)
            shutil.copyfileobj(response, destination)


def api_page(path):
    base = os.environ['SYSTEM_COLLECTIONURI'] + os.environ['SYSTEM_TEAMPROJECTID']
    request = urllib.request.Request(base + '/_apis/build/' + path,
        headers={'Authorization': 'Bearer ' + os.environ['SYSTEM_ACCESSTOKEN']})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response), response.headers.get('x-ms-continuationtoken')


def api(path):
    return api_page(path)[0]


def latest_run(component, development):
    source_component = 'firmware' if component == 'stress-firmware' else component
    query = dict(definitions=os.environ['KMS_' + source_component.upper() + '_PIPELINE_ID'],
                 statusFilter='completed', resultFilter='succeeded', queryOrder='finishTimeDescending',
                 **{'$top': 100, 'api-version': '7.1'})
    if development:
        query['branchName'] = 'refs/heads/master'
    while True:
        page, continuation = api_page('builds?' + urllib.parse.urlencode(query))
        for build in page['value']:
            if not development and not build['sourceBranch'].startswith('refs/tags/' + source_component + '/v'):
                continue
            available = api(f"builds/{build['id']}/artifacts?api-version=7.1")['value']
            if set(ARTIFACTS[component]).issubset(row['name'] for row in available):
                return build['id']
        if not continuation:
            break
        query['continuationToken'] = continuation
    source = 'master' if development else 'stable component release tags'
    raise ValueError(f'No successful {component} build with all required packages found on {source}. '
                     'Run that component pipeline first, then assemble downloads again.')


def resolve_selections(requested, development):
    selections = {}
    # Validate every input before querying Azure. Zero omits only optional products.
    for component, value in requested.items():
        value = str(value).strip()
        if value == '0' and component not in REQUIRED_COMPONENTS:
            continue
        if value != 'latest' and (not value.isascii() or not value.isdecimal() or int(value) <= 0):
            raise ValueError(f'{component}: enter latest or a positive numeric Azure build ID'
                             + ('; 0 skips this optional download' if component not in REQUIRED_COMPONENTS else ''))
        selections[component] = value
    if not all(component in selections for component in REQUIRED_COMPONENTS):
        raise ValueError('Firmware, Companion, Firmware Setup and Web Console are required')
    for component, value in selections.items():
        run_id = latest_run(component, development) if value == 'latest' else int(value)
        selections[component] = run_id
        print(f'{component}: selected Azure build ID {run_id}', flush=True)
    return selections


def extract_archive(archive, directory):
    with zipfile.ZipFile(archive) as package:
        names = set()
        entries = package.infolist()
        if len(entries) > 20000 or sum(e.file_size for e in entries) > 4 * 1024**3:
            raise ValueError('Artifact archive exceeds limits')
        for entry in entries:
            path = PurePosixPath(entry.filename)
            if (path.is_absolute() or '..' in path.parts or '\\' in entry.filename or ':' in entry.filename or
                    (entry.external_attr >> 16) & 0o170000 == 0o120000 or entry.filename.casefold() in names):
                raise ValueError('Unsafe artifact archive')
            names.add(entry.filename.casefold())
            if not entry.is_dir():
                target = directory.joinpath(*path.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with package.open(entry) as source, target.open('wb') as destination:
                    shutil.copyfileobj(source, destination)


def assemble(inputs, output, selections):
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError('Catalogue output must be empty')
    components, files, releases = {}, [], []
    for component, run_id in selections.items():
        identities = []
        for artifact in ARTIFACTS[component]:
            directory = inputs / artifact
            metadata = verify(directory, 'firmware' if component == 'stress-firmware' else component, run_id)
            identities.append((metadata['version'], metadata['commit']))
            shutil.copytree(directory, output / artifact)
            components.setdefault(component, dict(version=metadata['version'], commit=metadata['commit'],
                build_id=str(run_id), stable=metadata['stable'], artifacts=[]))['artifacts'].append(artifact)
            if component in ('firmware', 'stress-firmware'):
                index = json.loads((directory / 'release-index.json').read_text())
                releases.extend(dict(row, url=artifact + '/' + row['url']) for row in index['releases'])
        if len(set(identities)) != 1:
            raise ValueError('Platform packages for a component disagree')
    (output / 'release-index.json').write_text(json.dumps(dict(schema=1, releases=releases), indent=2) + '\n')
    lines = ['# KMSwitcher downloads', '', 'Install Firmware Setup and import the separate firmware ZIP. The Companion is optional.', '']
    for component, metadata in components.items():
        lines += [f"## {component} {metadata['version']}", '']
        for artifact in metadata['artifacts']:
            for path in sorted((output / artifact).iterdir()):
                if path.suffix in ('.exe', '.zip', '.deb', '.rpm', '.gz', '.html'):
                    lines.append(f'- [{path.name}]({path.relative_to(output).as_posix()})')
        lines.append('')
    (output / 'DOWNLOADS.md').write_text('\n'.join(lines), encoding='utf-8')
    for path in sorted(output.rglob('*')):
        if path.is_file():
            files.append(dict(path=path.relative_to(output).as_posix(), sha256=hashlib.sha256(path.read_bytes()).hexdigest(), size=path.stat().st_size))
    (output / 'release-manifest.json').write_text(json.dumps(dict(schema=2, components=components, files=files), indent=2) + '\n')
    manifest = output / 'release-manifest.json'
    files.append(dict(path=manifest.name, sha256=hashlib.sha256(manifest.read_bytes()).hexdigest()))
    (output / 'SHA256SUMS').write_text(''.join(f"{row['sha256']}  {row['path']}\n" for row in files))


def plan_downloads(selections, development):
    builds, downloads = [], []
    for component, run_id in selections.items():
        build = api(f'builds/{run_id}?api-version=7.1')
        source_component = 'firmware' if component == 'stress-firmware' else component
        definition = os.environ['KMS_' + source_component.upper() + '_PIPELINE_ID']
        if (build['status'] != 'completed' or build['result'] != 'succeeded' or
                str(build['definition']['id']) != definition):
            raise ValueError('Select a successful run of ' + component)
        if not development and not build['sourceBranch'].startswith('refs/tags/' + source_component + '/v'):
            raise ValueError('Select a stable component release or explicitly allow development artifacts')
        builds.append(build)
        available = api(f'builds/{run_id}/artifacts?api-version=7.1')['value']
        for artifact in ARTIFACTS[component]:
            row = next((r for r in available if r['name'] == artifact), None)
            if row is None:
                raise ValueError('Selected run has no packaged artifact: ' + artifact)
            request = artifact_request(row['resource']['downloadUrl'])
            downloads.append((component, run_id, artifact, request, build))
    return builds, downloads


def load_retention():
    spec = importlib.util.spec_from_file_location('retention', ROOT / 'scripts/publish-release.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def publish(selections, output, development=False):
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError('Catalogue output must be empty')
    selections = resolve_selections(selections, development)
    builds, downloads = plan_downloads(selections, development)
    module = load_retention()
    # Check permissions for the assembly run and every selected source before
    # transferring large installers. Probes remove only their temporary leases.
    module.retention(probe=True)
    for build in builds:
        module.retention(probe=True, run_id=build['id'], definition_id=build['definition']['id'])
    with tempfile.TemporaryDirectory(prefix='kms-catalogue-') as temporary:
        inputs = Path(temporary)
        for component, run_id, artifact, request, build in downloads:
            archive = inputs / (artifact + '.zip')
            print(f'{component}: downloading {artifact} from build {run_id}', flush=True)
            download_archive(request, archive, artifact)
            staging = inputs / ('extract-' + artifact)
            staging.mkdir()
            extract_archive(archive, staging)
            matches = list(staging.rglob('component.json'))
            if len(matches) != 1:
                raise ValueError('Artifact must contain one component manifest')
            shutil.move(str(matches[0].parent), inputs / artifact)
            source_component = 'firmware' if component == 'stress-firmware' else component
            metadata = verify(inputs / artifact, source_component, run_id)
            if metadata['commit'] != build['sourceVersion'] or (not development and not metadata['stable']):
                raise ValueError('Artifact source identity does not match its successful build')
        assemble(inputs, output, selections)
        # Protect each selected source run, not just the catalogue's copied files.
        for build in builds:
            module.retention(run_id=build['id'], definition_id=build['definition']['id'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for component in ARTIFACTS:
        optional = component not in REQUIRED_COMPONENTS
        parser.add_argument('--' + component + '-run', default='0' if optional else 'latest',
                            help='latest or an Azure build ID' + ('; 0 omits this optional download' if optional else ''))
    parser.add_argument('--development', action='store_true',
                        help='Allow development builds; latest uses master instead of stable release tags')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/catalogue')
    args = parser.parse_args()
    selections = {component: getattr(args, component.replace('-', '_') + '_run') for component in ARTIFACTS}
    publish(selections, args.output, args.development)
