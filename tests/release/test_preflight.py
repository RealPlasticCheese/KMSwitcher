"""Exercise prerequisite failures before expensive build work starts."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace
from unittest.mock import MagicMock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ci_preflight", ROOT / "scripts/ci-preflight.py")
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)


class PreflightTests(unittest.TestCase):
    def test_cache_probe_handles_new_and_nested_cache_without_changing_content(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory) / "cache"
            preflight.check_cache(cache)
            entry = cache / "http/private/entry"
            entry.parent.mkdir(parents=True)
            entry.write_bytes(b"cached wheel")
            preflight.check_cache(cache)
            self.assertEqual(entry.read_bytes(), b"cached wheel")
            self.assertEqual([path for path in cache.rglob("*") if path.is_file()], [entry])

    def test_cache_rejects_unreadable_files_and_directories(self):
        with tempfile.TemporaryDirectory() as directory:
            for kind in ("file", "directory"):
                with self.subTest(kind=kind):
                    def walk(path, onerror):
                        if kind == "directory":
                            onerror(PermissionError("private directory"))
                        yield directory, [], ["private-file"]
                    with patch.object(preflight.os, "walk", side_effect=walk), \
                         patch.object(Path, "open", side_effect=PermissionError("private file")):
                        with self.assertRaises(PermissionError):
                            preflight.check_cache(Path(directory))

    def test_missing_packaging_variable_is_rejected_before_docker_or_freezing(self):
        for value in ("", "$(KMS_FIRMWARE_PIPELINE_ID)", "0", "-12", "12.0", "abc", "²"):
            with self.subTest(value=value), patch.dict(os.environ, {"KMS_FIRMWARE_PIPELINE_ID": value}), \
                 patch.object(preflight, "command") as command:
                with self.assertRaisesRegex(ValueError, "positive definition ID"):
                    preflight.check_package("setup")
                command.assert_not_called()

    def test_setup_and_stress_require_collection_and_project(self):
        for component in ("setup", "stress"):
            for name in ("SYSTEM_COLLECTIONURI", "SYSTEM_TEAMPROJECTID"):
                environment = dict(KMS_FIRMWARE_PIPELINE_ID="12", SYSTEM_COLLECTIONURI="https://dev.azure.com/test/",
                                   SYSTEM_TEAMPROJECTID="project")
                environment[name] = "$(MissingVariable)"
                with self.subTest(component=component, name=name), patch.dict(os.environ, environment):
                    with self.assertRaisesRegex(ValueError, name):
                        preflight.check_package(component)

    def test_companion_does_not_require_firmware_configuration(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(preflight.sys, "platform", "win32"):
            preflight.check_package("companion")

    def test_linux_requires_an_available_docker_daemon(self):
        with patch.object(preflight.sys, "platform", "linux"), patch.object(preflight.shutil, "which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "requires Docker"):
                preflight.check_package("companion")
        with patch.object(preflight.sys, "platform", "linux"), patch.object(preflight.shutil, "which", return_value="docker"), \
             patch.object(preflight, "command", side_effect=subprocess.CalledProcessError(1, "docker")):
            with self.assertRaises(subprocess.CalledProcessError):
                preflight.check_package("companion")

    def test_python_checks_exercise_qt_in_a_child_process_and_stress_dependencies(self):
        with patch.object(preflight, "command") as command:
            preflight.check_python("stress")
        self.assertEqual(command.call_args_list[0].args[0][-2:], ["pip", "check"])
        smoke = command.call_args_list[1]
        self.assertIn("QApplication([])", smoke.args[0][-1])
        self.assertIn("import evdev", smoke.args[0][-1])
        self.assertIn("import pyudev", smoke.args[0][-1])
        self.assertEqual(smoke.kwargs["env"]["QT_QPA_PLATFORM"], "offscreen")

    def test_dependency_or_qt_failure_stops_preflight(self):
        for failure in (1, -6):
            with self.subTest(failure=failure), \
                 patch.object(preflight, "command", side_effect=subprocess.CalledProcessError(failure, "dependency")):
                with self.assertRaises(subprocess.CalledProcessError):
                    preflight.check_python("setup")

    def test_native_checks_compile_link_and_load_before_tests(self):
        compiler = MagicMock()
        compiler.compile_command.return_value = ["compiler"]
        compiler.link_command.return_value = ["linker"]
        module = SimpleNamespace(NativeCompiler=lambda: compiler)
        with patch.dict(sys.modules, native_build=module), patch.object(preflight, "command") as command:
            preflight.check_python("companion")
        compiler.compile_command.assert_called_once()
        compiler.link_command.assert_called_once()
        self.assertEqual(compiler.link_command.call_args.kwargs["exports"], ["kms_ci_probe"])
        self.assertIn("ctypes.CDLL", command.call_args.args[0][2])

    def test_missing_native_compiler_stops_preflight(self):
        module = SimpleNamespace(NativeCompiler=MagicMock(side_effect=RuntimeError("Install a native C compiler")))
        with patch.dict(sys.modules, native_build=module), patch.object(preflight, "command"):
            with self.assertRaisesRegex(RuntimeError, "native C compiler"):
                preflight.check_python("firmware")

    def test_sdk_checks_reporting_dependencies_and_both_compilers(self):
        with patch.object(preflight, "command") as command:
            preflight.check_sdk()
        commands = [call.args[0] for call in command.call_args_list]
        self.assertIn("import esp_idf_size", commands[0][-1])
        self.assertIn(["xtensa-esp32s2-elf-gcc", "--version"], commands)
        self.assertIn(["xtensa-esp32s3-elf-gcc", "--version"], commands)

    def test_commands_have_a_timeout_and_propagate_failure(self):
        with patch.object(preflight.subprocess, "run", side_effect=subprocess.TimeoutExpired("tool", 60)) as run:
            with self.assertRaises(subprocess.TimeoutExpired):
                preflight.command(["tool", "--version"])
        self.assertTrue(run.call_args.kwargs["check"])
        self.assertEqual(run.call_args.kwargs["timeout"], 60)

    def test_selected_python_is_checked_before_running_tools(self):
        with patch.object(preflight.sys, "version_info", (3, 9)), patch.object(preflight, "command") as command:
            with self.assertRaisesRegex(RuntimeError, "3.10"):
                preflight.check_python("setup")
            command.assert_not_called()

    def test_source_checks_each_component(self):
        with patch.object(preflight, "command"):
            for component in preflight.COMPONENTS:
                with self.subTest(component=component):
                    preflight.check_source(component)

    def test_pipeline_checks_run_before_expensive_work_and_container_cache_is_separate(self):
        import yaml
        application = yaml.safe_load((ROOT / "pipelines/templates/application.yml").read_text())
        package = application["stages"][1]
        linux = next(job for job in package["jobs"] if job.get("job") == "Linux")
        steps = linux["steps"]
        self.assertIn("--phase package", steps[1]["script"])
        cache = next(step["inputs"] for step in steps if step.get("task") == "Cache@2")
        self.assertIn("pip-container-v3", cache["key"])
        self.assertTrue(cache["path"].endswith("/pip-container-cache"))
        build = next(step["bash"] for step in steps if "bash" in step)
        self.assertLess(build.index("--phase cache"), build.index("docker run"))
        self.assertIn('trap \'chown -R "$KMS_CACHE_OWNER" /pip-cache /src/build\' EXIT', build)
        self.assertLess(build.index("chown -R 0:0 /pip-cache"), build.index("scripts/install-build-dependencies.py"))
        self.assertLess(build.index("python -m pip cache dir"), build.index("scripts/install-build-dependencies.py"))
        self.assertLess(build.index("--phase python"), build.index("scripts/build-companion.py"))
        windows = next(iter(package["jobs"][0].values()))[0]
        steps = json.dumps(windows["steps"])
        self.assertLess(steps.index("-PrepareOnly"), steps.index("scripts/build-companion.py"))
        identity = (ROOT / "pipelines/templates/identity.yml").read_text()
        self.assertLess(identity.index("ci-preflight.py"), identity.index("release-version.py"))
        self.assertIn("--retention-probe", identity)


if __name__ == "__main__":
    unittest.main()
