"""Check artifact authentication, redirects and failures before large downloads."""
import io
import os
from pathlib import Path
import tempfile
import unittest
import urllib.error
import urllib.request
import zipfile
from types import SimpleNamespace
from unittest.mock import Mock, patch

from test_components import script


class CatalogueDownloadTests(unittest.TestCase):
    def setUp(self):
        self.module = script('release-catalogue')
        environment = dict(SYSTEM_COLLECTIONURI='https://dev.azure.com/example/', SYSTEM_ACCESSTOKEN='test-token')
        environment.update({'KMS_' + name.upper() + '_PIPELINE_ID': str(value)
                            for name, value in dict(firmware=12, companion=13, setup=14, web=15).items()})
        self.environment = patch.dict(os.environ, environment)
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def test_collection_and_azure_artifact_service_receive_authentication(self):
        for url in ('https://dev.azure.com/example/artifact', 'https://dev.azure.com:443/example/artifact',
                    'https://artprodeau1.artifacts.visualstudio.com/project/content?format=zip'):
            with self.subTest(url=url):
                request = self.module.artifact_request(url)
                self.assertEqual(request.get_header('Authorization'), 'Bearer test-token')

    def test_signed_storage_and_untrusted_hosts_do_not_receive_token(self):
        for url in ('https://example.blob.core.windows.net/archive?sig=signed',
                    'https://external.example/archive.zip', 'https://dev.azure.com:444/archive.zip',
                    'https://artprodeau1.artifacts.visualstudio.com.evil.example/archive.zip',
                    'https://evilartifacts.visualstudio.com/archive.zip',
                    'https://artprodeau1.artifacts.visualstudio.com:444/archive.zip'):
            with self.subTest(url=url):
                self.assertIsNone(self.module.artifact_request(url).get_header('Authorization'))

    def test_insecure_or_credential_bearing_urls_are_rejected(self):
        for url in ('http://dev.azure.com/archive', 'file:///tmp/archive.zip',
                    'https://user:password@dev.azure.com/archive', 'https:///archive.zip'):
            with self.subTest(url=url), self.assertRaisesRegex(ValueError, 'HTTPS without URL credentials'):
                self.module.artifact_request(url)

    def test_same_origin_redirect_keeps_authentication(self):
        request = self.module.artifact_request('https://artprodeau1.artifacts.visualstudio.com/content')
        redirected = self.module.ArtifactRedirectHandler().redirect_request(
            request, None, 302, 'Found', {}, 'https://artprodeau1.artifacts.visualstudio.com:443/content?format=zip')
        self.assertEqual(redirected.get_header('Authorization'), 'Bearer test-token')

    def test_cross_origin_redirect_removes_authentication(self):
        request = self.module.artifact_request('https://artprodeau1.artifacts.visualstudio.com/content')
        for target in ('https://storage.blob.core.windows.net/archive?sig=signed',
                       'https://external.example/archive', 'https://other.artifacts.visualstudio.com/archive',
                       'https://artprodeau1.artifacts.visualstudio.com:444/archive'):
            with self.subTest(target=target):
                redirected = self.module.ArtifactRedirectHandler().redirect_request(
                    request, None, 302, 'Found', {}, target)
                self.assertIsNone(redirected.get_header('Authorization'))
        self.assertEqual(request.get_header('Authorization'), 'Bearer test-token')

    def test_https_redirect_cannot_downgrade_to_http(self):
        request = self.module.artifact_request('https://dev.azure.com/example/artifact')
        with self.assertRaisesRegex(ValueError, 'HTTPS'):
            self.module.ArtifactRedirectHandler().redirect_request(
                request, None, 302, 'Found', {}, 'http://dev.azure.com/archive')

    def test_valid_zip_is_downloaded_without_losing_its_signature(self):
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, 'w') as archive:
            archive.writestr('component.json', '{}')
        payload = stream.getvalue()
        request = self.module.artifact_request('https://dev.azure.com/example/artifact')
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(self.module.urllib.request, 'build_opener') as opener:
            opener.return_value.open.return_value = io.BytesIO(payload)
            target = Path(directory) / 'artifact.zip'
            self.module.download_archive(request, target, 'firmware')
            self.assertEqual(target.read_bytes(), payload)
            with zipfile.ZipFile(target) as archive:
                self.assertEqual(archive.namelist(), ['component.json'])

    def test_html_json_empty_and_truncated_responses_fail_after_four_bytes(self):
        request = self.module.artifact_request('https://dev.azure.com/example/artifact?sig=secret-signature')
        for payload in (b'<!doctype html>sign-in page', b'{"error":"sign in"}', b'', b'PK'):
            with self.subTest(payload=payload), tempfile.TemporaryDirectory() as directory, \
                    patch.object(self.module.urllib.request, 'build_opener') as opener:
                response = io.BytesIO(payload)
                response.headers = {'Content-Type': 'text/html'}
                opener.return_value.open.return_value = response
                target = Path(directory) / 'artifact.zip'
                with patch.object(response, 'read', wraps=response.read) as read:
                    with self.assertRaisesRegex(ValueError, 'firmware:.*did not return a ZIP.*authentication') as error:
                        self.module.download_archive(request, target, 'firmware')
                    read.assert_called_once_with(4)
                self.assertFalse(target.exists())
                self.assertNotIn('secret-signature', str(error.exception))
                self.assertNotIn('test-token', str(error.exception))

    def test_http_errors_do_not_create_an_archive(self):
        request = self.module.artifact_request('https://dev.azure.com/example/artifact')
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(self.module.urllib.request, 'build_opener') as opener:
            opener.return_value.open.side_effect = urllib.error.HTTPError(request.full_url, 401, 'Unauthorized', {}, None)
            target = Path(directory) / 'artifact.zip'
            with self.assertRaises(urllib.error.HTTPError):
                self.module.download_archive(request, target, 'firmware')
            self.assertFalse(target.exists())

    def selection_api(self, path):
        run_id = int(path.split('/')[1].split('?')[0])
        component = {12: 'firmware', 13: 'companion', 14: 'setup', 15: 'web'}[run_id]
        if '/artifacts?' in path:
            return {'value': [dict(name=name, resource={'downloadUrl': 'https://dev.azure.com/example/' + name})
                              for name in self.module.ARTIFACTS[component]]}
        return dict(id=run_id, status='completed', result='succeeded', definition={'id': run_id},
                    sourceBranch='refs/heads/master', sourceVersion='a' * 40)

    def test_missing_later_artifact_or_failed_run_prevents_all_downloads(self):
        for failure in ('missing-artifact', 'failed-run'):
            def api(path):
                response = self.selection_api(path)
                if failure == 'missing-artifact' and path.startswith('builds/15/artifacts?'):
                    return {'value': []}
                if failure == 'failed-run' and path.startswith('builds/15?'):
                    response['result'] = 'failed'
                return response
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory, \
                    patch.object(self.module, 'api', side_effect=api), \
                    patch.object(self.module, 'download_archive') as download:
                with self.assertRaises(ValueError):
                    self.module.publish(dict(firmware=12, companion=13, setup=14, web=15), Path(directory), True)
                download.assert_not_called()

    def test_retention_permission_is_checked_for_every_source_before_download(self):
        for failed_probe in range(5):
            retain = Mock(side_effect=[None] * failed_probe + [RuntimeError('Retention permission denied')])
            with self.subTest(failed_probe=failed_probe), tempfile.TemporaryDirectory() as directory, \
                    patch.object(self.module, 'api', side_effect=self.selection_api), \
                    patch.object(self.module, 'load_retention', return_value=SimpleNamespace(retention=retain)), \
                    patch.object(self.module, 'download_archive') as download:
                with self.assertRaisesRegex(RuntimeError, 'Retention permission denied'):
                    self.module.publish(dict(firmware=12, companion=13, setup=14, web=15), Path(directory), True)
                download.assert_not_called()
                self.assertTrue(all(call.kwargs['probe'] for call in retain.call_args_list))

    def test_nonempty_output_is_rejected_before_azure_queries(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(self.module, 'api') as api:
            output = Path(directory)
            (output / 'previous-download.zip').write_bytes(b'old')
            with self.assertRaisesRegex(ValueError, 'Catalogue output must be empty'):
                self.module.publish(dict(firmware=12, companion=13, setup=14, web=15), output, True)
            api.assert_not_called()
