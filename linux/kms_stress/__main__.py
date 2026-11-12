import argparse
from pathlib import Path
import signal
import sys
import threading
from . import __version__
from .model import Config, MAX_MIXED_SOAK_SECONDS, PRESETS


def run_main():
    parser = argparse.ArgumentParser(description="KMSwitcher Linux hardware test controller",
                                     epilog="Use 'kms-stress acceptance --help' for physical rig evidence.")
    parser.add_argument("--profile", type=Path, default=Path.home() / ".config/kmswitcher-stress/profile.json")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--run", action="store_true", help="Run the saved profile without the dashboard")
    mode.add_argument("--demo", action="store_true", help="Generate a hardware-free demo report")
    mode.add_argument("--discover", action="store_true", help="Read-only device discovery")
    mode.add_argument("--web", action="store_true", help="Start the browser dashboard without Tk")
    parser.add_argument("--open-browser", action="store_true", help="Open the browser dashboard on this machine")
    parser.add_argument("--host", default="127.0.0.1", help="Web listen address (use 0.0.0.0 for LAN access)")
    parser.add_argument("--port", type=int, default=8765, help="Web listen port (default: 8765)")
    parser.add_argument("--output", help="Override results directory")
    parser.add_argument("--preset", choices=PRESETS, help="Override the saved preset for --run or --web")
    parser.add_argument("--duration-seconds", type=int, help="Override duration for Soak / All tests, or per receiver for Mixed soak")
    parser.add_argument("--replay-recipe", type=Path, help="Repeat one saved USB campaign recipe with the configured fixture")
    args = parser.parse_args()
    try:
        config = Config.load(args.profile) if args.profile.exists() else Config()
        if args.output: config.output = args.output
        if (args.preset or args.replay_recipe or args.duration_seconds is not None) and not (args.run or args.web):
            raise ValueError("Preset/replay overrides require --run or --web")
        if args.preset: config.preset = args.preset
        if args.duration_seconds is not None:
            maximum = MAX_MIXED_SOAK_SECONDS if config.preset == "mixed-soak" else 604800
            if config.preset not in ("soak", "all-tests", "mixed-soak") or not 1 <= args.duration_seconds <= maximum:
                raise ValueError(f"Duration requires Soak, All tests or Mixed soak and must be 1–{maximum} seconds")
            config.soak_seconds = args.duration_seconds
        if args.replay_recipe:
            if args.replay_recipe.stat().st_size > 16384: raise ValueError("Campaign recipe is too large")
            config.campaign_recipe = args.replay_recipe.read_text(encoding="utf-8")
            config.preset = "usb-stall"
            config.validate_campaign()
        if args.web:
            from .web import serve
            serve(args.profile, config, args.host, args.port, open_browser=args.open_browser)
            return 0
        if args.demo:
            from .runner import demo
            result, path = demo(config)
            print(result["status"], path / "report.html")
            return 0 if result["status"] == "DEMO PASS" else 1
        if args.discover:
            from .hardware import discover_serial, receiver_nodes
            print(f"KMSwitcher hardware test {__version__}: read-only discovery", flush=True)
            def diagnostic(kind, data):
                print(f"{data['port']} [{kind}]: {data['line']}", flush=True)
            devices, problems = discover_serial(diagnostic)
            for role, device in devices: print(role, device.identity, device.uart)
            for identity, nodes in receiver_nodes().items(): print("receiver", identity, nodes.hid)
            for problem in problems: print("Unavailable:", problem)
            return 0
        if args.run:
            from .runner import Runner
            cancel = threading.Event()
            signal.signal(signal.SIGINT, lambda *_: cancel.set())
            signal.signal(signal.SIGTERM, lambda *_: cancel.set())
            result, _ = Runner(config, print, cancel).run()
            return 0 if result["status"] == "PASS" else 1
        from .gui import show
        show(args.profile)
        return 0
    except (ValueError, OSError, RuntimeError) as error:
        parser.exit(2, f"{error}\n")


def main():
    if sys.argv[1:2] == ["acceptance"]:
        from .acceptance import main as acceptance
        return acceptance(sys.argv[2:])
    # Source-checkout launchers and the frozen toolkit use the same hardware lock.
    shared = Path(__file__).resolve().parents[2] / "companion"
    if shared.exists(): sys.path.insert(0, str(shared))
    from kms_update.fixture_lock import fixture_lock
    from .power import inhibit_sleep
    try:
        packaged_session = getattr(sys, "frozen", False) and not any(arg in sys.argv for arg in ("--demo", "--discover", "--help"))
        with fixture_lock(), inhibit_sleep(enabled=packaged_session):
            return run_main()
    except RuntimeError as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
