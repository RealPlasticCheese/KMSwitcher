"""Bounded stream delivery shared by clipboard and firmware upload adapters."""
from .stream import Receipt, StreamLimits, send_stream

PROFILES = ("conservative", "balanced", "fast")
PROFILE_LABELS = ("Conservative (range and reliability)", "Balanced", "Fast (strong signal)")
