"""Bounded runtime logs and Python/native fault traces for packaged processes."""
import faulthandler
import logging
from logging.handlers import RotatingFileHandler
import os
import sys
import threading
from . import BUILD_INFO, __version__

_fault_stream = None


def configure(component):
    global _fault_stream
    from .agent import config_directory
    directory = config_directory() / 'logs'
    directory.mkdir(parents=True, exist_ok=True)
    logger = logging.getLogger('kms_companion')
    logger.setLevel(logging.INFO)
    handler = RotatingFileHandler(directory / f'{component}.log', maxBytes=1024 * 1024,
                                  backupCount=2, encoding='utf-8')
    handler.setFormatter(logging.Formatter('%(asctime)s %(levelname)s %(name)s: %(message)s'))
    logger.addHandler(handler)

    fault_path = directory / f'{component}-fault.log'
    if fault_path.exists() and fault_path.stat().st_size >= 1024 * 1024:
        fault_path.replace(fault_path.with_suffix('.previous.log'))
    _fault_stream = fault_path.open('a', encoding='utf-8', buffering=1)
    faulthandler.enable(file=_fault_stream, all_threads=True)
    if sys.stderr is None:
        sys.stderr = _fault_stream

    def exception(kind, value, traceback):
        logger.critical('Unhandled main/UI exception', exc_info=(kind, value, traceback))

    def thread_exception(args):
        logger.critical('Unhandled exception in thread %s', args.thread.name,
                        exc_info=(args.exc_type, args.exc_value, args.exc_traceback))

    sys.excepthook = exception
    threading.excepthook = thread_exception
    logger.info('Starting %s pid=%s version=%s commit=%s', component, os.getpid(),
                __version__, BUILD_INFO.get('commit', 'source checkout'))
    return directory
