"""Windowed launcher, independent of the companion and its login startup."""
import os
import sys


def main():
    if sys.stdout is None:
        sys.stdout = open(os.devnull, "w")
    if sys.stderr is None:
        sys.stderr = open(os.devnull, "w")
    from .__main__ import main as run
    return run()
