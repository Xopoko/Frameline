#!/bin/sh
set -eu

export PATH="$SNAP/usr/bin:$PATH"
export LD_LIBRARY_PATH="$SNAP/opt/qt6/lib:$SNAP/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$SNAP/opt/qt6/plugins"
export XDG_DATA_DIRS="$SNAP/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export FONTCONFIG_PATH="$SNAP/etc/fonts"

case "${WAYLAND_DISPLAY:-}" in
    ""|/*) ;;
    *)
        # snapd gives the app a private runtime directory. Keep it private for IPC.
        if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -S "${XDG_RUNTIME_DIR%/*}/$WAYLAND_DISPLAY" ]; then
            export WAYLAND_DISPLAY="${XDG_RUNTIME_DIR%/*}/$WAYLAND_DISPLAY"
        fi
        ;;
esac

exec "$@"
