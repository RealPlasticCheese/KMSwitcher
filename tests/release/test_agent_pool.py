"""Check the independent Windows and Linux pools and their hosted fallbacks."""
from pathlib import Path
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[2]


class AgentPoolTests(unittest.TestCase):
    def test_python_setup_matches_the_selected_agent(self):
        steps = yaml.safe_load((ROOT / 'pipelines/templates/python.yml').read_text())['steps']
        hosted = steps[0]['${{ if eq(parameters.selfHosted, false) }}']
        self.assertEqual([step['task'] for step in hosted], ['UsePythonVersion@0', 'Cache@2'])
        local_windows = steps[1]["${{ if and(parameters.selfHosted, eq(parameters.platform, 'windows')) }}"]
        self.assertIn('python -m venv', local_windows[0]['pwsh'])
        install = steps[2]["${{ if or(not(parameters.selfHosted), eq(parameters.platform, 'windows')) }}"][0]
        self.assertEqual(install['script'], 'python scripts/install-build-dependencies.py')
        self.assertNotIn('condition', install)
        local_linux = steps[3]["${{ if and(parameters.selfHosted, eq(parameters.platform, 'linux')) }}"]
        self.assertIn('prepare-python.py', local_linux[0]['bash'])

    def test_templates_do_not_depend_on_unavailable_agent_detection_variable(self):
        for path in (ROOT / 'pipelines').rglob('*.yml'):
            with self.subTest(path=path.name):
                self.assertNotIn('Agent.IsHosted', path.read_text())

    def test_early_preflight_failure_does_not_publish_missing_log_directory(self):
        steps = yaml.safe_load((ROOT / 'pipelines/templates/results.yml').read_text())['steps']
        self.assertIn("os.path.isdir('build/test-logs')", steps[1]['script'])
        self.assertEqual(steps[1]['condition'], 'always()')
        self.assertIn("eq(variables['hasTestLogs'], 'true')", steps[2]['condition'])

    def test_entry_points_expose_manual_azure_fallback(self):
        for name in ('firmware', 'companion', 'setup', 'stress', 'web', 'catalogue'):
            with self.subTest(component=name):
                path = ROOT / ('azure-pipelines.yml' if name == 'catalogue' else f'pipelines/{name}.yml')
                pipeline = yaml.safe_load(path.read_text())
                choice = next(p for p in pipeline['parameters'] if p['name'] == 'runLinuxOnAzure')
                self.assertIs(choice['default'], False)
                if 'extends' in pipeline:
                    self.assertEqual(pipeline['extends']['parameters']['runLinuxOnAzure'], '${{ parameters.runLinuxOnAzure }}')
                if name in ('companion', 'setup'):
                    windows_choice = next(p for p in pipeline['parameters'] if p['name'] == 'runWindowsOnAzure')
                    self.assertIs(windows_choice['default'], False)
                    self.assertEqual(pipeline['extends']['parameters']['runWindowsOnAzure'], '${{ parameters.runWindowsOnAzure }}')

    def test_windows_and_linux_jobs_choose_their_own_pool(self):
        def visit(value):
            if isinstance(value, dict):
                if 'job' in value and 'pool' in value:
                    pool = value['pool']
                    if value['job'] == 'Windows':
                        self.assertEqual(pool['${{ if parameters.runWindowsOnAzure }}']['vmImage'], 'windows-2022')
                        self.assertEqual(pool['${{ else }}']['name'], 'SelfHosted-Windows')
                        self.assertEqual(pool['${{ else }}']['demands'], ['Agent.OS -equals Windows_NT'])
                    else:
                        self.assertEqual(pool['${{ else }}']['name'], 'SelfHosted-Linux')
                        self.assertEqual(pool['${{ if eq(parameters.runLinuxOnAzure, true) }}']['vmImage'], 'ubuntu-22.04')
                for child in value.values(): visit(child)
            elif isinstance(value, list):
                for child in value: visit(child)
        for name in ('pipelines/templates/application.yml', 'pipelines/firmware.yml', 'pipelines/web.yml', 'azure-pipelines.yml'):
            visit(yaml.safe_load((ROOT / name).read_text()))

    def test_sdk_sources_survive_checkout_cleanup_on_nuc(self):
        pipeline = yaml.safe_load((ROOT / 'pipelines/firmware.yml').read_text())
        settings = pipeline['jobs'][0]['variables']['${{ if eq(parameters.runLinuxOnAzure, false) }}']
        self.assertTrue(settings['KMS_IDF_SOURCE_ROOT'].startswith('$(Agent.WorkFolder)/'))
        self.assertIn('KMS_IDF_SOURCE_ROOT', (ROOT / 'scripts/setup-toolchain.sh').read_text())
