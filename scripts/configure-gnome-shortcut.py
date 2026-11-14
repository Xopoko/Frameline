#!/usr/bin/python3
"""Opt in to replacing GNOME's Super+S with Frameline, or restore it."""

import argparse
import fcntl
import json
import os
from pathlib import Path
import shlex
import shutil
import sys
import tempfile

from gi.repository import Gio, GLib


MEDIA = "org.gnome.settings-daemon.plugins.media-keys"
SHELL = "org.gnome.shell.keybindings"
LIST = "custom-keybindings"
# Frameline was called ScreenFrame. These identifiers keep that name so an existing assignment can
# still be found and removed.
CUSTOM = "/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/screenframe-region/"
SHELL_KEYS = ("toggle-quick-settings", "toggle-overview")
BINDING = "<Super>s"


def is_super_s(binding):
    return binding.casefold() in ("<super>s", "<mod4>s")


def custom_settings(path):
    return Gio.Settings.new_with_path(f"{MEDIA}.custom-keybinding", path)


def user_value(settings, key):
    value = settings.get_user_value(key)
    return None if value is None else value.unpack()


def set_value(settings, key, value, signature):
    if value is None:
        settings.reset(key)
    elif not settings.set_value(key, GLib.Variant(signature, value)):
        raise ValueError(f"GNOME rejected the change to {key}.")
    Gio.Settings.sync()
    if user_value(settings, key) != value:
        raise ValueError(f"GNOME did not save the change to {key}.")


def check_other_custom_bindings(media):
    for path in media.get_strv(LIST):
        if path != CUSTOM and is_super_s(custom_settings(path).get_string("binding")):
            raise ValueError("Another custom shortcut already uses Super+S.")


def assignment_matches(media, custom, record):
    return (CUSTOM in media.get_strv(LIST)
            and all(custom.get_string(key) == value for key, value in record["custom"].items()))


def status(media, shell, custom, record_path):
    if record_path.exists():
        record = json.loads(record_path.read_text(encoding="utf-8"))
        if not assignment_matches(media, custom, record):
            print("The saved Frameline shortcut differs from the GNOME settings.")
            return 1
        if any(user_value(shell, key) != change["after"] for key, change in record["shell"].items()):
            print("The previous GNOME shortcut was edited after Frameline was configured.")
            return 1
        check_other_custom_bindings(media)
        print("Super+S is assigned to Frameline.")
        return 0
    if CUSTOM in media.get_strv(LIST):
        print("A Frameline shortcut exists without saved settings.")
        return 1
    print("Super+S takeover is not configured.")
    return 1


def install(media, shell, custom, record_path, executable):
    if record_path.exists():
        record = json.loads(record_path.read_text(encoding="utf-8"))
        if (assignment_matches(media, custom, record)
                and all(user_value(shell, key) == change["after"]
                        for key, change in record["shell"].items())):
            check_other_custom_bindings(media)
            print("Super+S already launches Frameline.")
            return
        raise ValueError("An incomplete Frameline shortcut assignment exists. Run remove first.")

    if not shutil.which(executable) or not Path(executable).is_absolute():
        raise ValueError("Specify the absolute path to the installed frameline executable.")
    check_other_custom_bindings(media)
    if CUSTOM in media.get_strv(LIST) or any(
            user_value(custom, key) is not None for key in ("name", "command", "binding")):
        raise ValueError("A Frameline shortcut exists without saved settings.")

    changes = {}
    schema = shell.get_property("settings-schema")
    for key in SHELL_KEYS:
        if not schema.has_key(key):
            continue
        current = shell.get_strv(key)
        remaining = [value for value in current if not is_super_s(value)]
        if remaining != current:
            changes[key] = {"before": user_value(shell, key), "after": remaining}
    values = {
        "name": "Frameline: Capture region",
        "command": f"{shlex.quote(executable)} capture region",
        "binding": BINDING,
    }
    writable = [(media, LIST), *((shell, key) for key in changes),
                *((custom, key) for key in values)]
    if not all(settings.is_writable(key) for settings, key in writable):
        raise ValueError("Some GNOME shortcuts are read-only.")

    paths = media.get_strv(LIST)
    record = {"shell": changes, "custom": values, "paths": paths, "userPaths": user_value(media, LIST)}
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=record_path.parent) as file:
        json.dump(record, file, ensure_ascii=False)
        file.flush()
        os.fsync(file.fileno())
        os.link(file.name, record_path)
    try:
        for key, change in changes.items():
            set_value(shell, key, change["after"], "as")
        for key, value in values.items():
            set_value(custom, key, value, "s")
        set_value(media, LIST, media.get_strv(LIST) + [CUSTOM], "as")
    except ValueError:
        remove(media, shell, custom, record_path)
        raise
    print("Super+S now opens Frameline region capture.")


def remove(media, shell, custom, record_path):
    if not record_path.exists():
        raise ValueError("No saved Frameline shortcut assignment was found.")
    record = json.loads(record_path.read_text(encoding="utf-8"))
    for key, value in record["custom"].items():
        if user_value(custom, key) not in (None, value):
            raise ValueError("The Frameline shortcut was edited. Remove it in GNOME Settings.")
    for key, change in record["shell"].items():
        if user_value(shell, key) not in (change["before"], change["after"]):
            raise ValueError(f"The {key} shortcut was edited. Restore it in GNOME Settings.")
    if record["shell"]:
        check_other_custom_bindings(media)
    paths = media.get_strv(LIST)
    if CUSTOM in paths:
        remaining = [path for path in paths if path != CUSTOM]
        set_value(media, LIST, record["userPaths"] if remaining == record["paths"] else remaining, "as")
    for key in record["custom"]:
        set_value(custom, key, None, "s")
    for key, change in record["shell"].items():
        set_value(shell, key, change["before"], "as")
    record_path.unlink()
    print("Frameline shortcut removed and the previous Super+S assignment restored.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("install", "remove", "status"),
                        help="install replaces Super+S, remove restores it, status only checks")
    parser.add_argument("--executable", default="/usr/bin/frameline",
                        help="absolute path to Frameline (for install)")
    args = parser.parse_args()
    source = Gio.SettingsSchemaSource.get_default()
    if not source or not all(source.lookup(schema, True)
                             for schema in (MEDIA, f"{MEDIA}.custom-keybinding", SHELL)):
        parser.error("GNOME shortcut schemas are unavailable.")
    if not os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        parser.error("Run this command in the GNOME user session.")
    if os.geteuid() == 0:
        parser.error("Run this command without sudo.")

    state = Path(os.environ.get("XDG_STATE_HOME") or Path.home() / ".local/state") / "screenframe"
    record_path = state / "gnome-region-shortcut.json"
    settings = Gio.Settings.new(MEDIA), Gio.Settings.new(SHELL), custom_settings(CUSTOM)
    if args.action == "status":
        sys.exit(status(*settings, record_path))

    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    with (state / "gnome-region-shortcut.lock").open("a", encoding="utf-8") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if args.action == "install":
            install(*settings, record_path, args.executable)
        else:
            remove(*settings, record_path)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
