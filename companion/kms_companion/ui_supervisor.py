"""Recover a crashed tray process without restarting its HID owner or the hardware."""
import logging
import subprocess
import time

logger = logging.getLogger(__name__)


def run(command, *, spawn=subprocess.Popen, sleep=time.sleep, clock=time.monotonic):
    failures = []
    while True:
        process = spawn(command)
        code = process.wait()
        if code == 0:
            logger.info('Tray process pid=%s exited normally', process.pid)
            return 0  # Quit, session shutdown and activation of an existing UI are final.
        now = clock()
        failures = [at for at in failures if now - at < 60]
        failures.append(now)
        logger.error('Tray process pid=%s exited unexpectedly: code=%s', process.pid, code)
        if len(failures) > 3:
            logger.critical('Tray restart limit reached; retaining fault logs for diagnosis')
            return code
        delay = 2 ** (len(failures) - 1)
        logger.info('Restarting tray in %s seconds', delay)
        sleep(delay)
