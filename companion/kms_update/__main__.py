"""Standalone maintenance commands; never starts the edge-switching helper."""
import argparse
from contextlib import redirect_stdout
import json
import os
from pathlib import Path
import sys
import zipfile

from . import firmware, management
from .fixture_lock import fixture_lock


def open_controller(port):
    import serial
    connection = serial.Serial()
    connection.port, connection.baudrate = port, 115200
    connection.timeout, connection.write_timeout = 0.1, 5
    connection.dtr = connection.rts = False
    connection.open()
    return connection


def execute(args):
    if args.command == "check":
        return firmware.Bundle(args.bundle, product=args.product).validate()
    if args.command == "ports":
        return firmware.ports()
    stored = args.command == "ota" and getattr(args, "cached", False)
    if stored and args.role != "receiver":
        raise ValueError("Stored S2 firmware can update receivers only; leave the controller unchecked")
    application = getattr(args, 'application', None)
    bundle = None
    if args.command not in ("inspect", "restart") and not stored and application is None:
        bundle = firmware.Bundle(args.bundle, product=args.product)
        bundle.validate()
    if args.command == "flash":
        if args.role not in bundle.document["roles"]:
            raise ValueError("Selected role is not included in this firmware bundle")
        return firmware.flash(args.role, args.port, bundle.directory, expected_mac=args.expected_mac,
                              identify=args.identify, erase=args.erase)
    if args.command in ('ota', 'store-receiver') and not stored:
        role = 'receiver' if args.command == 'store-receiver' else args.role
        image, metadata = (management.load_firmware(application, role) if application is not None
                           else bundle.application(role))  # Validate before opening a port.
        receiver_image = None
        if role == 'controller' and (args.slot or args.all):
            if application is not None: raise ValueError('A local controller image can only update the controller')
            receiver_image = bundle.application('receiver')
    else:
        image = metadata = receiver_image = None
    with open_controller(args.port) as connection:
        controller = management.Controller(connection, speed_profile=getattr(args, "speed", "conservative"))
        if args.command == 'store-receiver':
            outcome = management.stage_cache(controller, image, metadata,
                progress=lambda done, total: print(f'Storing receiver firmware: {done * 100 // total}%'))
            return dict(cache_result=outcome, receiver_cache=management.cache_identity(controller))
        if args.command == "restart":
            targets = controller.peers()[0] if args.all else args.slot
            return management.restart_targets(controller, targets)
        if args.command == "inspect":
            peers, selected = controller.peers()
            devices = []
            for slot in [0, *peers]:
                try:
                    identity = controller.identity(slot)
                    row = dict(slot=slot, identity=identity)
                    if identity['capabilities'] & management.CAP_RADIO_DIAGNOSTICS:
                        try: row['radio'] = controller.radio_status(slot, identity)
                        except (management.ManagementError, ValueError, OSError) as error: row['radio_error'] = str(error)
                    devices.append(row)
                except management.ManagementError as error:
                    devices.append(dict(slot=slot, error=str(error)))
            result = dict(devices=devices, selected=selected, receiver_cache=None)
            try:
                result['receiver_cache'] = management.cache_identity(controller)
            except (management.ManagementError, ValueError, OSError) as error:
                result['receiver_cache_error'] = str(error)
            return result
        if args.command == "ota":
            results = []
            if args.role == "receiver" or receiver_image is not None:
                peers, selected = controller.peers()
                targets = peers if args.all else args.slot
                if not targets or any(slot not in peers for slot in targets):
                    raise ValueError("Select one or more paired receivers")
                if stored:
                    receiver_metadata = management.cache_identity(controller)
                    if not receiver_metadata:
                        raise ValueError("No verified S2 firmware is stored on this controller")
                    expected = getattr(args, 'cached_sha256', None)
                    if expected and expected != receiver_metadata['sha256']:
                        raise ValueError("Stored S2 firmware changed since inspection; inspect again before updating")
                    receiver_data, cached = b'', True
                else:
                    receiver_data, receiver_metadata = receiver_image or (image, metadata)
                    cached = bool(controller.identity(0)['capabilities'] & 8)
                    if cached:
                        management.stage_cache(controller, receiver_data, receiver_metadata)
                results = management.update_targets(controller, targets, selected, receiver_data, receiver_metadata, args.force, cached=cached)
            if args.role == "controller":
                results.append(management.update_master(controller, image, metadata, args.force))
            return dict(results=results, success=all(row["result"] in ("updated", "skipped") and
                row.get('receiver_cache') != 'failed' for row in results))
    raise ValueError("Unknown maintenance action")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    identity_path = firmware.default_bundle().parent / "release.json"
    identity = json.loads(identity_path.read_text()) if getattr(sys, "frozen", False) and identity_path.exists() else {}
    parser.add_argument("--product", choices=("regular", "stress"), default="stress" if identity.get("product") == "stress" else "regular")
    parser.add_argument("--bundle", type=Path)
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("check")
    sub.add_parser("ports")
    ui = sub.add_parser("ui")
    ui.add_argument('--task', choices=('restart',))
    cache = sub.add_parser('store-receiver', help='Store verified receiver firmware on the controller without updating a dongle')
    cache.add_argument('--port', required=True)
    cache.add_argument('--application', type=Path)
    restart = sub.add_parser("restart", help="Restart paired dongles through the S3 controller")
    restart.add_argument("--port", required=True)
    targets = restart.add_mutually_exclusive_group(required=True)
    targets.add_argument("--slot", type=int, choices=range(1, 8), action="append")
    targets.add_argument("--all", action="store_true")
    for name in ("flash", "inspect", "ota"):
        action = sub.add_parser(name)
        action.add_argument("--port", required=True)
        if name != "inspect":
            action.add_argument("--role", choices=("controller", "receiver", "generator") if name == "flash" else ("controller", "receiver"), required=True)
        if name == "flash":
            action.add_argument("--expected-mac")
            mode = action.add_mutually_exclusive_group()
            mode.add_argument("--identify", action="store_true", help="Read the selected board identity without writing")
            mode.add_argument("--erase", action="store_true", help="Erase all flash before writing; requires --expected-mac")
        if name == "ota":
            target = action.add_mutually_exclusive_group()
            target.add_argument("--slot", type=int, choices=range(1, 8), action="append")
            target.add_argument("--all", action="store_true")
            action.add_argument("--force", action="store_true")
            action.add_argument("--speed", choices=("conservative", "balanced", "fast"), default="conservative")
            action.add_argument("--cached", action="store_true", help="Use the verified S2 firmware stored on the controller; no local image is uploaded")
            action.add_argument("--cached-sha256", help="Require the stored image shown during inspection")
            action.add_argument('--application', type=Path, help='Local application .bin with a matching .json manifest')
    args = parser.parse_args(argv)
    if args.command == 'ota':
        if args.cached and args.application:
            parser.error('choose stored firmware or a local application image')
        if args.cached and args.role != 'receiver':
            parser.error('--cached requires --role receiver')
        if args.cached_sha256 and (not args.cached or len(args.cached_sha256) != 64 or
                                  any(char not in '0123456789abcdef' for char in args.cached_sha256)):
            parser.error('--cached-sha256 requires --cached and a lowercase SHA-256 hash')
    try:
        if args.bundle and args.bundle.is_file():
            from .releases import install_archive
            args.bundle = install_archive(args.bundle, args.product)
        if args.command in (None, "ui"):
            from .ui import launch
            task = getattr(args, 'task', None) or os.environ.get('KMS_SETUP_INITIAL_TASK')
            return launch(args.product, args.bundle, initial_task=task)
        with redirect_stdout(sys.stderr):
            if args.command in ("inspect", "ota", "restart", "store-receiver"):
                with fixture_lock():
                    result = execute(args)
            else:
                result = execute(args)
        print(json.dumps(result))
        return 1 if isinstance(result, dict) and result.get("success") is False else 0
    except (OSError, ValueError, RuntimeError, KeyError, ImportError, zipfile.BadZipFile, management.ManagementError) as error:
        print(json.dumps({"error": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
