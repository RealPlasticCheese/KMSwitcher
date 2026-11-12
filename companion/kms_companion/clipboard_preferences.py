"""Clipboard policies shared by settings, validated content and the tray."""

CONTENT_LABELS = {"text": "Plain text", "rich": "Formatted text", "image": "Images", "files": "Files and folders"}
RECEIVE_LABELS = {"clipboard": "Put on clipboard automatically", "manual": "Accept manually in Transfers", "save": "Save automatically to folder"}
DEFAULT_RECEIVING = {"text": "clipboard", "rich": "clipboard", "image": "manual", "files": "manual"}
AUTOMATIC_MODES = {"off": "Off", "selected": "Selected computers", "all": "All available paired computers"}
DEFAULT_AUTOMATIC = dict.fromkeys(CONTENT_LABELS, "off")


def content_type(manifest):
    if manifest["roots"]:
        return "files"
    formats = manifest["formats"]
    if "image/png" in formats:
        return "image"
    if any(name in formats for name in ("text/html", "text/rtf", "application/rtf")):
        return "rich"
    return "text"


def receiving_settings(settings):
    receiving = settings.get("receiving", {})
    if not isinstance(receiving, dict) or any(key not in CONTENT_LABELS for key in receiving):
        raise ValueError("Choose a receiving behaviour for each clipboard type")
    receiving = dict(DEFAULT_RECEIVING, **receiving)
    if any(not isinstance(value, str) or value not in RECEIVE_LABELS for value in receiving.values()):
        raise ValueError("Choose a valid receiving behaviour")
    folder = settings.get("save_folder", "")
    if not isinstance(folder, str) or ("save" in receiving.values() and not folder.strip()):
        raise ValueError("Choose a folder for automatically saved clipboard items")
    notifications = settings.get("arrival_popups", {})
    if not isinstance(notifications, dict) or any(
            kind not in CONTENT_LABELS or type(enabled) is not bool for kind, enabled in notifications.items()):
        raise ValueError("Choose whether to show an arrival popup for each clipboard type")
    return dict(receiving=receiving, save_folder=folder.strip(),
                arrival_popups={kind: notifications.get(kind, True) for kind in CONTENT_LABELS})


def automatic_modes(settings):
    modes = settings.get("automatic_send", {})
    if isinstance(modes, str):
        # The first version stored one choice for all types.
        modes = dict.fromkeys(CONTENT_LABELS, modes)
    if not isinstance(modes, dict) or any(
            kind not in CONTENT_LABELS or not isinstance(mode, str) or mode not in AUTOMATIC_MODES
            for kind, mode in modes.items()):
        raise ValueError("Choose a valid automatic sending mode for each clipboard type")
    return DEFAULT_AUTOMATIC | modes


def sending_settings(settings):
    modes = automatic_modes(settings)
    targets = settings.get("automatic_targets", {})
    if not isinstance(targets, dict) or any(not isinstance(identifier, str) or len(identifier) != 32 or
           any(c not in "0123456789abcdef" for c in identifier) or
           not isinstance(name, str) or not name or len(name.encode()) > 48
           for identifier, name in targets.items()):
        raise ValueError("Choose valid computers for automatic clipboard sending")
    return dict(automatic_send=modes, automatic_targets=dict(targets))
