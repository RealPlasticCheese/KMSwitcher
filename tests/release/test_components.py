import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
import shutil
import tarfile
import urllib.parse
import zipfile
from unittest.mock import patch
from fnmatch import fnmatchcase

ROOT = Path(__file__).resolve().parents[2]


def script(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / (name + '.py'))
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


class DownloadSelectionTests(unittest.TestCase):
    def setUp(self):
        self.module = script('release-catalogue')

    def test_latest_skips_source_only_and_incomplete_packages_across_pages(self):
        module = self.module
        with patch.dict(os.environ, KMS_COMPANION_PIPELINE_ID='13'), \
                patch.object(module, 'api_page', side_effect=[
                    ({'value': [{'id': 40}, {'id': 39}]}, 'next page'),
                    ({'value': [{'id': 38}]}, None)]) as pages, \
                patch.object(module, 'api', side_effect=[
                    {'value': []}, {'value': [{'name': 'windows-companion'}]},
                    {'value': [{'name': name} for name in module.ARTIFACTS['companion']]}]):
            self.assertEqual(module.latest_run('companion', development=True), 38)
        first = urllib.parse.parse_qs(pages.call_args_list[0].args[0].split('?', 1)[1])
        second = urllib.parse.parse_qs(pages.call_args_list[1].args[0].split('?', 1)[1])
        self.assertEqual(first['definitions'], ['13'])
        self.assertEqual(first['branchName'], ['refs/heads/master'])
        self.assertEqual(first['resultFilter'], ['succeeded'])
        self.assertEqual(first['statusFilter'], ['completed'])
        self.assertEqual(first['queryOrder'], ['finishTimeDescending'])
        self.assertEqual(second['continuationToken'], ['next page'])

    def test_stable_latest_ignores_branches_and_other_component_tags(self):
        module = self.module
        builds = [{'id': 40, 'sourceBranch': 'refs/heads/master'},
                  {'id': 39, 'sourceBranch': 'refs/tags/setup/v1.0.0'},
                  {'id': 38, 'sourceBranch': 'refs/tags/companion/v1.0.0'}]
        with patch.dict(os.environ, KMS_COMPANION_PIPELINE_ID='13'), \
                patch.object(module, 'api_page', return_value=({'value': builds}, None)) as pages, \
                patch.object(module, 'api', return_value={
                    'value': [{'name': name} for name in module.ARTIFACTS['companion']]}) as artifacts:
            self.assertEqual(module.latest_run('companion', development=False), 38)
        self.assertNotIn('branchName', pages.call_args.args[0])
        artifacts.assert_called_once_with('builds/38/artifacts?api-version=7.1')

    def test_diagnostic_latest_uses_firmware_pipeline_and_requires_extra_artifact(self):
        module = self.module
        with patch.dict(os.environ, KMS_FIRMWARE_PIPELINE_ID='12'), \
                patch.object(module, 'api_page', return_value=({'value': [{'id': 40}, {'id': 39}]}, None)) as pages, \
                patch.object(module, 'api', side_effect=[
                    {'value': [{'name': 'firmware'}]}, {'value': [{'name': 'stress-firmware'}]}]):
            self.assertEqual(module.latest_run('stress-firmware', development=True), 39)
        self.assertIn('definitions=12', pages.call_args.args[0])

    def test_no_packaged_build_explains_what_to_run(self):
        module = self.module
        with patch.dict(os.environ, KMS_SETUP_PIPELINE_ID='14'), \
                patch.object(module, 'api_page', return_value=({'value': []}, None)):
            with self.assertRaisesRegex(ValueError, 'No successful setup build.*Run that component pipeline first'):
                module.latest_run('setup', development=True)

    def test_defaults_resolve_latest_and_leave_optional_downloads_out(self):
        import yaml
        module = self.module
        document = yaml.safe_load((ROOT / 'azure-pipelines.yml').read_text())
        parameters = {row['name']: row for row in document['parameters']}
        requested = {component: parameters['stressFirmwareRun' if component == 'stress-firmware' else component + 'Run']['default']
                     for component in module.ARTIFACTS}
        self.assertTrue(parameters['allowDevelopment']['default'])
        self.assertTrue(all(row.get('displayName') for row in parameters.values()))
        with patch.object(module, 'latest_run', side_effect=[12, 13, 14, 15]) as latest:
            self.assertEqual(module.resolve_selections(requested, True),
                             dict(firmware=12, companion=13, setup=14, web=15))
        self.assertEqual([call.args for call in latest.call_args_list],
                         [(component, True) for component in module.REQUIRED_COMPONENTS])

    def test_explicit_ids_remain_pinned_and_optional_latest_is_supported(self):
        module = self.module
        requested = dict(firmware=' 12 ', companion='13', setup='14', web='15',
                         stress='latest', **{'stress-firmware': '17'})
        with patch.object(module, 'latest_run', return_value=16) as latest:
            self.assertEqual(module.resolve_selections(requested, False),
                             dict(firmware=12, companion=13, setup=14, web=15,
                                  stress=16, **{'stress-firmware': 17}))
        latest.assert_called_once_with('stress', False)

    def test_invalid_selections_fail_before_any_azure_query(self):
        module = self.module
        with patch.object(module, 'latest_run') as latest:
            for value in ('0', '-1', '1.2.3', 'firmware-123', '', '１２３'):
                with self.subTest(value=value), self.assertRaisesRegex(ValueError, 'positive numeric Azure build ID'):
                    module.resolve_selections(dict(firmware='latest', companion=value, setup='latest', web='latest'), True)
            with self.assertRaisesRegex(ValueError, 'are required'):
                module.resolve_selections({'firmware': 'latest'}, True)
        latest.assert_not_called()

    def test_explicit_build_still_requires_success_and_correct_pipeline(self):
        module = self.module
        for result, definition in [('failed', 12), ('succeeded', 99)]:
            with self.subTest(result=result, definition=definition), \
                    tempfile.TemporaryDirectory() as directory, \
                    patch.dict(os.environ, KMS_FIRMWARE_PIPELINE_ID='12'), \
                    patch.object(module, 'api', return_value=dict(status='completed', result=result,
                                                                 definition={'id': definition})):
                with self.assertRaisesRegex(ValueError, 'Select a successful run of firmware'):
                    module.publish(dict(firmware=12, companion=13, setup=14, web=15), Path(directory), True)

    def test_stable_mode_rejects_explicit_development_build(self):
        module = self.module
        with tempfile.TemporaryDirectory() as directory, \
                patch.dict(os.environ, KMS_FIRMWARE_PIPELINE_ID='12'), \
                patch.object(module, 'api', return_value=dict(status='completed', result='succeeded',
                    definition={'id': 12}, sourceBranch='refs/heads/master')):
            with self.assertRaisesRegex(ValueError, 'Select a stable component release'):
                module.publish(dict(firmware=12, companion=13, setup=14, web=15), Path(directory), False)

    def test_latest_builds_flow_through_download_verification_and_assembly(self):
        module = self.module
        recorder = script('component-artifact')
        retention = script('publish-release')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            selected = dict(firmware=12, companion=13, setup=14, web=15)
            archives = {}
            for component, run_id in selected.items():
                for artifact in module.ARTIFACTS[component]:
                    folder = root / artifact
                    folder.mkdir()
                    (folder / 'download.zip').write_bytes(b'example download')
                    if component == 'firmware':
                        (folder / 'release-index.json').write_text('{"releases": []}')
                    recorder.record(folder, dict(component=component, build_id=str(run_id),
                        version=f'1.0.0-dev.{run_id}', commit='a' * 40, stable=False))
                    stream = io.BytesIO()
                    with zipfile.ZipFile(stream, 'w') as archive:
                        for path in folder.iterdir():
                            archive.write(path, artifact + '/' + path.name)
                    archives[artifact] = stream.getvalue()

            def api(path):
                run_id = int(path.split('/')[1].split('?')[0])
                component = next(name for name, value in selected.items() if value == run_id)
                if '/artifacts?' in path:
                    return {'value': [dict(name=name, resource={'downloadUrl': 'https://example.com/' + name})
                                      for name in module.ARTIFACTS[component]]}
                return dict(id=run_id, status='completed', result='succeeded', definition={'id': run_id},
                            sourceBranch='refs/heads/master', sourceVersion='a' * 40)

            variables = {'KMS_' + name.upper() + '_PIPELINE_ID': str(value) for name, value in selected.items()}
            variables['SYSTEM_COLLECTIONURI'] = 'https://dev.azure.com/example/'
            with patch.dict(os.environ, variables), \
                    patch.object(module, 'latest_run', side_effect=selected.values()), \
                    patch.object(module, 'api', side_effect=api), \
                    patch.object(module.urllib.request, 'build_opener') as opener, \
                    patch.object(module, 'load_retention', return_value=retention), \
                    patch.object(retention, 'retention') as retain:
                opener.return_value.open.side_effect = lambda request, **kwargs: io.BytesIO(
                    archives[request.full_url.rsplit('/', 1)[1]])
                module.publish({name: 'latest' for name in selected}, root / 'output', True)
                self.assertEqual(retain.call_count, 9)
                self.assertTrue(retain.call_args_list[0].kwargs['probe'])
                self.assertTrue(all(call.kwargs.get('probe') for call in retain.call_args_list[1:5]))
                self.assertTrue(all('probe' not in call.kwargs for call in retain.call_args_list[5:]))
            manifest = json.loads((root / 'output/release-manifest.json').read_text())
            self.assertEqual({name: row['build_id'] for name, row in manifest['components'].items()},
                             {name: str(value) for name, value in selected.items()})
            self.assertTrue((root / 'output/DOWNLOADS.md').exists())


class ComponentTests(unittest.TestCase):
    def test_diagnostic_bundle_archives_an_already_published_regular_bundle(self):
        from test_firmware import make_bundle
        module = script('package-test-firmware')
        recorder = script('component-artifact')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            production = root / 'regular'; production.mkdir()
            document = make_bundle(production)
            identity = dict(component='firmware', version=document['version'], commit=document['commit'], stable=False)
            recorder.firmware_zip(production, identity)
            recorder.record(production, identity)
            generator = root / 'generator'; (generator / 'config').mkdir(parents=True)
            (generator / 'project_description.json').write_text(json.dumps({'project_version': document['commit'][:12]}))
            (generator / 'config/sdkconfig.json').write_text(json.dumps({'IDF_TARGET': 'esp32s3'}))
            flash_files = {}
            for item in document['roles']['controller']['files']:
                if item['path'].endswith('ota.bin'):
                    continue
                name = Path(item['path']).name
                shutil.copy2(production / item['path'], generator / name)
                flash_files[hex(item['offset'])] = name
            (generator / 'flasher_args.json').write_text(json.dumps({
                'flash_files': flash_files, 'flash_settings': document['roles']['controller']['settings']}))
            output = root / 'diagnostic'
            module.package(production, generator, output)
            self.assertFalse(list(output.glob('*.zip')))
            self.assertFalse((output / 'component.json').exists())
            self.assertFalse((output / 'release-index.json').exists())
            recorder.firmware_zip(output, identity, 'stress')
            recorder.record(output, identity)
            self.assertEqual(len(list(output.glob('*.zip'))), 1)
            index = json.loads((output / 'release-index.json').read_text())
            self.assertEqual(index['releases'][0]['product'], 'stress')

    def test_changes_trigger_only_dependent_components(self):
        components = json.loads((ROOT / 'pipelines/components.json').read_text())
        def triggered(path):
            return {name for name, config in components.items() if any(fnmatchcase(path, pattern) for pattern in config['paths'])}
        self.assertEqual(triggered('companion/kms_companion/ui.py'), {'companion'})
        self.assertEqual(triggered('web/styles.css'), {'web'})
        self.assertEqual(triggered('firmware/receiver/main/main.c'), {'firmware'})
        self.assertEqual(triggered('companion/kms_update/cache.py'), {'firmware', 'setup', 'stress', 'web'})
        self.assertEqual(triggered('firmware/components/kms_core/include/kms_protocol.h'), {'firmware', 'companion', 'setup', 'stress', 'web'})
        self.assertEqual(triggered('docs/configuration.md'), set())
        self.assertEqual(triggered('scripts/release-catalogue.py'), {'setup'})
        self.assertEqual(triggered('scripts/ci-preflight.py'), {'firmware', 'companion', 'setup', 'stress', 'web'})

    def test_component_tag_cannot_release_another_component(self):
        identity = script('release-version').identity
        self.assertTrue(identity('1.2.3', 'refs/tags/firmware/v1.2.3', '12', 'a'*40, 'firmware')['stable'])
        with self.assertRaises(ValueError):
            identity('1.2.3', 'refs/tags/firmware/v1.2.3', '12', 'a'*40, 'setup')

    def test_applications_have_no_firmware_pipeline_dependency_and_keep_installer_checks(self):
        import yaml
        for component in ('companion', 'setup', 'stress'):
            pipeline = yaml.safe_load((ROOT / f'pipelines/{component}.yml').read_text())
            self.assertTrue(pipeline['trigger']['batch'])
            self.assertNotIn('idf.ps1', json.dumps(pipeline))
            self.assertNotIn('download: firmware', json.dumps(pipeline))
            template = yaml.safe_load((ROOT / 'pipelines/templates/application.yml').read_text())
            package = template['stages'][1]
            jobs = {job['job']: job for job in package['jobs'] if 'job' in job}
            self.assertEqual(jobs['LinuxInstallers']['dependsOn'], 'Linux')
            self.assertEqual(len(jobs['LinuxInstallers']['strategy']['matrix']), 4)
            self.assertEqual(package['dependsOn'], 'Validate')
            self.assertEqual(package['condition'], 'succeeded()')
            self.assertNotIn('buildPackages', json.dumps(pipeline))
            self.assertNotIn('ci-selection.py', json.dumps(template))

    def test_catalogue_accepts_independent_versions_and_detects_modified_artifacts(self):
        module = script('release-catalogue'); recorder = script('component-artifact')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); inputs = root / 'inputs'
            for artifact in module.ARTIFACTS['companion']:
                folder = inputs / artifact; folder.mkdir(parents=True)
                (folder / 'application.zip').write_bytes(b'companion')
                recorder.record(folder, dict(component='companion', build_id='11', version='2.0.0', commit='a'*40, stable=True))
            web = inputs / 'console'; web.mkdir()
            (web / 'KMSwitcher.html').write_text('console')
            recorder.record(web, dict(component='web', build_id='22', version='3.0.0', commit='b'*40, stable=True))
            module.assemble(inputs, root / 'output', {'companion':11, 'web':22})
            document = json.loads((root / 'output/release-manifest.json').read_text())
            self.assertEqual(document['components']['web']['version'], '3.0.0')
            self.assertEqual(document['components']['companion']['version'], '2.0.0')
            for entry in document['files']:
                self.assertEqual(hashlib.sha256((root / 'output' / entry['path']).read_bytes()).hexdigest(), entry['sha256'])
            (web / 'KMSwitcher.html').write_text('modified')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                module.verify(web, 'web', 22)
            with self.assertRaisesRegex(ValueError, 'component/run'):
                module.verify(web, 'web', 23)

    def test_linux_tool_packages_generate_upgrade_cleanup_without_embedded_firmware(self):
        module = script('package-tools')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('packaging/linux/reload-udev.sh', 'companion/70-kmswitcher.rules',
                         'docs/installing.md', 'scripts/package-linux.py'):
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / name, destination)
            app = root / 'app'; app.mkdir()
            (app / 'kms-setup').write_text('test executable')
            module.ROOT = root
            identity = dict(version='1.2.3', deb_version='1.2.3', rpm_version='1.2.3', rpm_release='1')
            # Native packagers are external platform tools. Exercise all staging,
            # lifecycle-script generation and archive creation on either OS.
            with patch.object(module.subprocess, 'run') as native:
                module.package(app, identity, root / 'output', 'setup')
            self.assertEqual(native.call_count, 2)
            spec = (root / 'build/linux-package-setup/rpm/SPECS/kmswitcher-setup.spec').read_text()
            self.assertIn('rm -rf -- /opt/kmswitcher-setup/firmware', spec)
            archive = next((root / 'output').glob('*.tar.gz'))
            with tarfile.open(archive) as package:
                installer = package.extractfile('kmswitcher-setup/install.sh').read().decode()
                self.assertIn('rm -rf -- /opt/kmswitcher-setup/firmware', installer)
                self.assertNotIn('rm -rf -- /home', installer)
