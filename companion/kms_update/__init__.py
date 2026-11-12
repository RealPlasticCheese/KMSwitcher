"""Device maintenance shared by standalone setup and the separate test toolkit."""
import os
from pathlib import Path


def config_directory():
    variable = "LOCALAPPDATA" if os.name == "nt" else "XDG_CONFIG_HOME"
    return Path(os.environ.get(variable, Path.home() / ".config")) / "KMSwitcher"
