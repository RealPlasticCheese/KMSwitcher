"""Exercise the HTTP dashboard, real demo evidence, and runner cancellation."""
from dataclasses import asdict
from datetime import datetime, timedelta, timezone
import http.client
import io
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
import zipfile

from kms_stress.discovery import discover_config
from kms_stress.model import Config, Device
from kms_stress.runner import Runner, demo
from kms_stress.run_progress import RunProgress
from kms_stress.restarts import RestartCancelled
from kms_stress.web import Controller, ReportStore, WebServer


class WebTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.config = Config(output=str(self.root / "results"))
        self.controller = Controller(self.root / "profile.json", self.config)
        self.server = WebServer(("127.0.0.1", 0), self.controller)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        self.thread.start()
        self.addCleanup(self.close)

    def close(self):
        self.server.shutdown()
        self.controller.close()
        self.server.server_close()
        self.thread.join(5)

    def request(self, path, data=None, headers=None, raw=None):
        connection = http.client.HTTPConnection(*self.server.server_address, timeout=10)
        request_headers = {}
        method, body = "GET", None
        if data is not None or raw is not None:
            method, body = "POST", raw if raw is not None else json.dumps(data)
            request_headers.update({"Content-Type": "application/json", "X-KMS-Request": "1"})
        request_headers.update(headers or {})
        try:
            connection.request(method, path, body=body, headers=request_headers)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def json(self, path, data=None, **kwargs):
        status, _, body = self.request(path, data, **kwargs)
        return status, json.loads(body)

    def wait_worker(self):
        self.controller.worker.join(10)
        self.assertFalse(self.controller.worker.is_alive())

    def test_run_requires_exactly_one_selected_master(self):
        data = asdict(self.config)
        for masters in ([], [Device("112233445566", uart="one", enabled=False)],
                        [Device("112233445566", uart="one"), Device("AABBCCDDEEFF", uart="two")]):
            data["masters"] = [asdict(device) for device in masters]
            status, result = self.json("/api/run", data)
            self.assertEqual(status, 400)
            self.assertIn("exactly one master", result["error"])
            self.assertIsNone(self.controller.worker)

    def test_master_list_profile_round_trip(self):
        data = asdict(self.config)
        data["masters"] = [asdict(Device("112233445566", uart="one", enabled=False)),
                           asdict(Device("AABBCCDDEEFF", uart="two"))]
        self.assertEqual(self.json("/api/profile", data)[0], 200)
        restored = Config.load(self.controller.profile)
        self.assertEqual(len(restored.select_master()), 1)
        self.assertEqual(restored.master.uart, "two")

    def test_initial_state_is_idle_and_never_opens_hardware(self):
        with patch("kms_stress.hardware.SerialChannel") as serial:
            status, state = self.json("/api/state")
        self.assertEqual(status, 200)
        self.assertFalse(state["busy"])
        self.assertEqual(state["config"], asdict(self.config))
        serial.assert_not_called()

    def test_progress_timestamps_are_synthetic_once_on_every_line(self):
        moment = datetime(2026, 10, 26, 14, 47, 38, tzinfo=timezone(timedelta(hours=10)))
        with patch("kms_stress.web.datetime") as clock:
            clock.now.return_value.astimezone.return_value = moment
            self.controller.notify("Progress\n\nDiagnostic detail\n")
        expected = ("[2026-10-26 14:47:38+10:00] Progress\n"
                    "[2026-10-26 14:47:38+10:00] \n"
                    "[2026-10-26 14:47:38+10:00] Diagnostic detail")
        self.assertEqual(self.json("/api/state")[1]["messages"][-1], expected)
        self.assertEqual(self.json("/api/state")[1]["messages"][-1], expected)

    def test_top_progress_summary_is_above_the_fixture(self):
        status, _, body = self.request("/")
        self.assertEqual(status, 200)
        self.assertLess(body.index(b'id="run-summary"'), body.index(b'id="configuration"'))

    def test_unsaved_soak_preview_uses_server_rules_without_opening_or_saving_fixture(self):
        data = asdict(self.config)
        data.update(preset="soak", rates=[30], seconds=60)
        data["generators"] = [asdict(Device("AABBCCDDEE01")), asdict(Device("AABBCCDDEE02"))]
        with patch("kms_stress.hardware.SerialChannel") as serial:
            status, preview = self.json("/api/preview", data)
        self.assertEqual(status, 200)
        self.assertEqual(preview["workload"]["rates_per_generator"], [30])
        self.assertEqual(preview["workload"]["combined_rates"], [60])
        self.assertIn("single rate", preview["text"])
        self.assertEqual(self.controller.config.preset, "quick")
        self.assertFalse(self.controller.profile.exists())
        self.assertIsNone(self.controller.worker)
        serial.assert_not_called()

    def test_timed_suite_preview_and_live_progress_without_hardware(self):
        data = asdict(self.config)
        data.update(preset="all-tests", rates=[10, 100], soak_seconds=28800, recovery_attempts=3)
        status, preview = self.json("/api/preview", data)
        self.assertEqual(status, 200)
        self.assertEqual(preview["workload"]["soak_seconds"], 28800)
        self.assertIn("timed loop", preview["text"])
        runner = Runner(self.config)
        runner.suite_progress = dict(running=False, cycle=2, test="pointer", remaining_seconds=120, counts={"PASS": 4, "FAIL": 1})
        self.controller.runner = runner
        status, state = self.json("/api/state")
        self.assertEqual(status, 200)
        self.assertEqual(state["progress"]["counts"], {"PASS": 4, "FAIL": 1})

    def test_soak_countdown_survives_reconnecting_to_the_state_api(self):
        runner = Runner(self.config)
        runner.run_progress = RunProgress(10800, "soak", timed=True)
        runner.run_progress.start_timed(10900)
        self.controller.runner = runner
        with patch("kms_stress.run_progress.time.monotonic", return_value=100):
            self.assertEqual(self.json("/api/state")[1]["progress"]["remaining_seconds"], 10800)
        with patch("kms_stress.run_progress.time.monotonic", return_value=580):
            state = self.json("/api/state")[1]
        self.assertEqual(state["progress"]["remaining_seconds"], 10320)
        self.assertFalse(state["progress"]["estimated"])
        self.assertEqual(state["progress"]["counts"], {})

    def test_dashboard_and_api_open_without_a_token_or_session_cookie(self):
        for path in ("/", "/api/state", "/api/reports", "/app.js"):
            status, headers, _ = self.request(path)
            self.assertEqual(status, 200)
            self.assertNotIn("Set-Cookie", headers)
        self.assertEqual(self.request("/?token=old-session")[0], 200)
        self.assertEqual(self.json("/api/profile", asdict(self.config))[0], 200)

    def test_assets_serve_without_tk_or_external_resources(self):
        for path, expected in (("/", b"Saved reports"), ("/app.js", b"/api/reports"), ("/style.css", b"@media")):
            status, headers, body = self.request(path)
            self.assertEqual(status, 200)
            self.assertIn(expected, body)
            self.assertEqual(headers["X-Content-Type-Options"], "nosniff")
            self.assertIn("default-src 'self'", headers["Content-Security-Policy"])

    def test_post_rejects_missing_request_header_and_cross_origin(self):
        for kwargs in ({"headers": {"X-KMS-Request": ""}},
                       {"headers": {"Origin": "https://untrusted.example"}}):
            self.assertEqual(self.json("/api/demo", asdict(self.config), **kwargs)[0], 403)
        self.assertIsNone(self.controller.worker)

    def test_save_unfinished_profile_and_restore_it(self):
        data = asdict(self.config)
        data["generators"] = [asdict(Device("", "Unconfigured", enabled=False))]
        status, _ = self.json("/api/profile", data)
        self.assertEqual(status, 200)
        restored = Config.load(self.controller.profile)
        self.assertEqual(restored.generators[0].name, "Unconfigured")
        self.assertFalse(restored.generators[0].enabled)

    def test_invalid_json_and_field_types_return_useful_errors(self):
        self.assertEqual(self.json("/api/profile", raw="{")[0], 400)
        self.assertEqual(self.json("/api/profile", raw="null")[0], 400)
        self.assertEqual(self.json("/api/profile", raw="[]")[0], 400)
        for key, value in (("seconds", True), ("rates", [1001]), ("rates", "10"), ("max_log_mb", "1024"),
                           ("output", ""), ("output", "invalid\0path"), ("master", None), ("stop_on_failure", 1)):
            data = {**asdict(self.config), key: value}
            status, result = self.json("/api/profile", data)
            self.assertEqual(status, 400, (key, result))
            self.assertIn("error", result)
        self.assertFalse(self.controller.profile.exists())

    def test_incomplete_fixture_cannot_start_hardware_test(self):
        status, result = self.json("/api/run", asdict(self.config))
        self.assertEqual(status, 400)
        self.assertIn("master", result["error"])
        self.assertIsNone(self.controller.worker)

    def test_demo_lists_real_report_and_downloads_complete_evidence_zip(self):
        with patch("kms_stress.hardware.SerialChannel") as serial:
            self.assertEqual(self.json("/api/demo", asdict(self.config))[0], 200)
            self.wait_worker()
        serial.assert_not_called()
        status, listing = self.json("/api/reports")
        self.assertEqual(status, 200)
        self.assertEqual(len(listing["reports"]), 1)
        report = listing["reports"][0]
        self.assertEqual(report["status"], "DEMO PASS")
        self.assertEqual(self.json("/api/state")[1]["report"], report["name"])
        status, headers, body = self.request(f"/reports/{report['name']}/download.zip")
        self.assertEqual(status, 200)
        self.assertEqual(headers["Content-Type"], "application/zip")
        self.assertIn("attachment;", headers["Content-Disposition"])
        self.assertEqual(int(headers["Content-Length"]), len(body))
        directory = Path(self.config.output) / report["name"]
        with zipfile.ZipFile(io.BytesIO(body)) as archive:
            for path in directory.rglob("*"):
                if path.is_file():
                    self.assertEqual(archive.read(f"{report['name']}/{path.relative_to(directory).as_posix()}"), path.read_bytes())
        status, headers, body = self.request(f"/reports/{report['name']}/report.html")
        self.assertEqual(status, 200)
        self.assertIn(b"DEMO PASS", body)
        self.assertIn("sandbox;", headers["Content-Security-Policy"])

    def test_existing_reports_survive_controller_restart_and_output_changes(self):
        self.assertEqual(self.json("/api/demo", asdict(self.config))[0], 200)
        self.wait_worker()
        name = self.controller.snapshot()["report"]
        restored = Controller(self.controller.profile, self.config)
        self.assertEqual(restored.report_store().list()["reports"][0]["name"], name)
        data = {**asdict(self.config), "output": str(self.root / "other")}
        self.assertEqual(self.json("/api/profile", data)[0], 200)
        self.assertEqual(self.json("/api/reports")[1]["reports"], [])
        self.assertIsNone(self.json("/api/state")[1]["report"])
        self.assertEqual(self.request(f"/reports/{name}/download.zip")[0], 404)

    def test_report_traversal_and_unfinished_runs_are_not_exported(self):
        unfinished = Path(self.config.output) / "unfinished"
        unfinished.mkdir(parents=True)
        (unfinished / "capture.jsonl").write_text("partial capture")
        self.assertEqual(self.json("/api/reports")[1]["reports"], [])
        for name in ("..", "%2e%2e%2foutside", "%2e%2e%5coutside", "unfinished", "missing"):
            self.assertEqual(self.request(f"/reports/{name}/download.zip")[0], 404)
        self.assertEqual(self.request("/../profile.json")[0], 404)

    def test_runner_stop_preserves_cancelled_report_and_rejects_concurrent_work(self):
        config = Config(master=Device("112233445566", uart="master"),
                        generators=[Device("AABBCCDDEE01", uart="generator")],
                        receivers=[Device("AABBCCDDEE02", slot=1)], output=self.config.output)
        entered = threading.Event()

        def preflight(runner):
            entered.set()
            runner.cancel.wait(5)
            runner.check()

        with patch.object(Runner, "preflight", preflight):
            self.assertEqual(self.json("/api/run", asdict(config))[0], 200)
            self.assertTrue(entered.wait(5))
            for action in ("demo", "discover", "run", "profile"):
                self.assertEqual(self.json(f"/api/{action}", asdict(config))[0], 409)
            # Browser polling and old-report browsing remain available while hardware is busy.
            self.assertTrue(self.json("/api/state")[1]["busy"])
            self.assertEqual(self.json("/api/reports")[0], 200)
            self.assertEqual(self.json("/api/stop", {})[0], 200)
            self.wait_worker()
        report = self.json("/api/reports")[1]["reports"][0]
        self.assertEqual(report["status"], "CANCELLED")
        self.assertEqual(self.request(f"/reports/{report['name']}/download.zip")[0], 200)

    def test_restart_is_serialized_cancellable_and_preserves_the_last_report(self):
        config = Config(master=Device("112233445566", uart="master"),
                        generators=[Device("AABBCCDDEE01", uart="generator")],
                        receivers=[Device("AABBCCDDEE02", "Office receiver", slot=1)], output=self.config.output)
        target = {"role":"receiver", "identity":"AABBCCDDEE02"}
        entered = threading.Event()
        observed = []
        self.controller.report = "previous-report"
        def restart(config, notify, cancel, target=None):
            observed.append(target)
            entered.set()
            cancel.wait(5)
            raise RestartCancelled("Device restart cancelled")
        with patch("kms_stress.web.restart_devices", side_effect=restart):
            self.assertEqual(self.json("/api/restart", {"config":asdict(config), "target":target})[0], 200)
            self.assertTrue(entered.wait(5))
            state = self.json("/api/state")[1]
            self.assertTrue(state["busy"])
            self.assertEqual(state["operation"], "restart")
            self.assertEqual(state["report"], "previous-report")
            for action in ("run", "demo", "discover", "profile"):
                self.assertEqual(self.json(f"/api/{action}", asdict(config))[0], 409)
            self.assertEqual(self.json("/api/restart", {"config":asdict(config), "target":target})[0], 409)
            self.assertEqual(self.json("/api/stop", {})[0], 200)
            self.wait_worker()
        self.assertEqual(observed, [target])
        self.assertIn("Device restart cancelled", self.controller.snapshot()["messages"][-1])

    def test_restart_rejects_unknown_devices_and_nonboolean_pre_run_option(self):
        with patch("kms_stress.web.restart_devices") as restart:
            self.assertEqual(self.json("/api/restart", {"config":asdict(self.config),
                "target":{"role":"receiver", "identity":"AABBCCDDEE02"}})[0], 400)
            self.assertEqual(self.json("/api/profile", {**asdict(self.config), "restart_before_run":1})[0], 400)
        self.assertIsNone(self.controller.worker)
        restart.assert_not_called()

    def test_close_cancels_and_waits_for_worker_cleanup(self):
        entered, finished = threading.Event(), threading.Event()

        def discover(config, notify, cancel):
            entered.set()
            cancel.wait(5)
            finished.set()
            return config

        with patch("kms_stress.web.discover_config", discover):
            self.json("/api/discover", asdict(self.config))
            self.assertTrue(entered.wait(5))
            self.controller.close()
        self.assertTrue(finished.is_set())
        self.assertFalse(self.controller.worker.is_alive())
        self.assertEqual(self.json("/api/demo", asdict(self.config))[0], 409)

    def test_worker_error_is_visible_and_next_operation_can_run(self):
        with patch("kms_stress.web.discover_config", side_effect=RuntimeError("UART unavailable")):
            self.json("/api/discover", asdict(self.config))
            self.wait_worker()
        state = self.json("/api/state")[1]
        self.assertIn("UART unavailable", "\n".join(state["messages"]))
        self.assertFalse(state["busy"])
        self.assertEqual(self.json("/api/demo", asdict(self.config))[0], 200)
        self.wait_worker()

    def test_archive_excludes_symlinks_outside_report(self):
        _, directory = demo(self.config)
        secret = self.root / "secret.txt"
        secret.write_text("not report evidence")
        try:
            (directory / "link.txt").symlink_to(secret)
            (directory / "external").symlink_to(self.root, target_is_directory=True)
        except OSError as error:
            self.skipTest(f"Symlink creation unavailable: {error}")
        with ReportStore(self.config.output).archive(directory.name) as stream:
            with zipfile.ZipFile(stream) as archive:
                self.assertFalse(any("link.txt" in name or "external" in name for name in archive.namelist()))


class DiscoveryTests(unittest.TestCase):
    def test_discovery_recreates_removed_master_and_keeps_alternatives(self):
        config = Config(master=Device("000000000001", uart="removed"), masters=[])
        found = [("master", Device("112233445566", uart="new"))]
        with patch("kms_stress.discovery.discover_serial", return_value=(found, [])), \
             patch("kms_stress.discovery.receiver_nodes", return_value={}), \
             patch("kms_stress.discovery.SerialChannel"), \
             patch("kms_stress.discovery.read_identity", return_value=type("Identity", (), {"role": "master", "identity": "112233445566"})()), \
             patch("kms_stress.discovery.peers", return_value=({}, {}, None, [])):
            result = discover_config(config, lambda message: None, threading.Event())
        self.assertEqual([d.identity for d in result.masters], ["112233445566"])
        self.assertEqual(result.master.uart, "new")
        result.masters[0].enabled = False
        found.append(("master", Device("AABBCCDDEEFF", uart="other")))
        with patch("kms_stress.discovery.discover_serial", return_value=(found, [])), \
             patch("kms_stress.discovery.receiver_nodes", return_value={}), \
             patch("kms_stress.discovery.SerialChannel") as serial:
            result = discover_config(result, lambda message: None, threading.Event())
        self.assertEqual(len(result.masters), 2)
        self.assertFalse(any(d.enabled for d in result.masters))
        serial.assert_not_called()

    def test_shared_discovery_retains_names_slots_and_mismatched_identities(self):
        config = Config(master=Device("112233445566", uart="old-master"),
                        generators=[Device("AABBCCDDEE01", "Saved generator", "old-uart", enabled=False)],
                        receivers=[Device("AABBCCDDEE02", "Saved receiver", slot=2)])
        found = [("master", Device("112233445566", uart="new-master")),
                 ("generator", Device("AABBCCDDEE01", uart="new-uart")),
                 ("generator", Device("AABBCCDDEE03", uart="third-uart"))]
        messages = []
        with patch("kms_stress.discovery.discover_serial", return_value=(found, [])), \
             patch("kms_stress.discovery.receiver_nodes", return_value={"AABBCCDDEE05": object()}), \
             patch("kms_stress.discovery.SerialChannel") as serial, \
             patch("kms_stress.discovery.read_identity", return_value=type("Identity", (), {"role": "master", "identity": "112233445566"})()), \
             patch("kms_stress.discovery.peers", return_value=({2: "AABBCCDDEE04", 5: "AABBCCDDEE05"}, {}, None, [])):
            discovered = discover_config(config, messages.append, threading.Event())
        self.assertEqual(config.master.uart, "old-master")
        self.assertEqual(discovered.master.uart, "new-master")
        self.assertEqual(discovered.generators[0].name, "Saved generator")
        self.assertFalse(discovered.generators[0].enabled)
        self.assertEqual(discovered.generators[0].uart, "new-uart")
        self.assertEqual(discovered.receivers[0].identity, "AABBCCDDEE02")
        self.assertTrue(discovered.receivers[1].enabled)
        self.assertEqual(discovered.receivers[1].slot, 5)
        self.assertTrue(any("saved assignment retained" in message for message in messages))
        serial.return_value.close.assert_called_once()

    def test_web_cli_does_not_import_desktop_gui(self):
        from kms_stress.__main__ import main
        with tempfile.TemporaryDirectory() as directory:
            args = ["kms-stress", "--web", "--profile", str(Path(directory) / "profile.json"), "--host", "0.0.0.0", "--port", "9000"]
            with patch("sys.argv", args), patch("kms_stress.web.serve") as serve, patch.dict("sys.modules", {"tkinter": None}):
                self.assertEqual(main(), 0)
            self.assertEqual(serve.call_args.args[2:], ("0.0.0.0", 9000))


if __name__ == "__main__":
    unittest.main()
