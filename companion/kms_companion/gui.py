"""Console-free companion entry point; maintenance is a separate on-demand tool."""
import logging
import os
import sys

UI_EXCEPTION_EXIT = 70


def main():
    arguments = sys.argv[1:]
    worker = bool(arguments and arguments[0] == '--ui-worker')
    if worker:
        arguments = arguments[1:]
    smoke = os.environ.get("KMS_COMPANION_SMOKE") == "1"
    from .runtime_log import configure
    try:
        directory = configure('tray' if worker or smoke else 'tray-launcher')
    except OSError:
        # A full/read-only profile must not prevent access to the switcher.
        directory = None
    if not worker and not smoke:
        from .ui_supervisor import run
        command = [sys.executable]
        if not getattr(sys, 'frozen', False):
            command += ['-m', 'kms_companion.gui']
        return run([*command, '--ui-worker', *arguments])
    from PySide6.QtWidgets import QApplication, QMessageBox
    from .cli import main as cli_main
    log_exception = sys.excepthook
    def ui_exception(kind, value, traceback):
        log_exception(kind, value, traceback)
        app = QApplication.instance()
        if app:
            app.exit(UI_EXCEPTION_EXIT)  # Recover instead of continuing with partial state.
    sys.excepthook = ui_exception
    if not arguments:
        arguments = ["ui"]
    # Windowed PyInstaller launches have no stdio streams.
    if sys.stdout is None:
        sys.stdout = open(os.devnull, "w")
    if sys.stderr is None:
        sys.stderr = open(os.devnull, "w")
    if smoke:
        from .ui import smoke_test
        smoke_test()
        return 0
    try:
        result = cli_main(arguments)
        if result and result != UI_EXCEPTION_EXIT:
            app = QApplication.instance() or QApplication(sys.argv[:1])
            QMessageBox.critical(None, "KMSwitcher", "Unable to start KMSwitcher. Run kmswitcher from a terminal for diagnostics.")
        return result
    except Exception as error:
        logging.getLogger(__name__).exception('Tray startup or event loop failed')
        app = QApplication.instance() or QApplication(sys.argv[:1])
        detail = f'\nDiagnostic logs: {directory}' if directory else ''
        QMessageBox.critical(None, "KMSwitcher", str(error) + detail)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
