#!/bin/bash
set -euo pipefail
umask 077

if [[ $# != 0 || $EUID == 0 ]]; then
    echo "Run bash scripts/install-ubuntu-library.sh as the Ubuntu user, without sudo." >&2
    exit 2
fi

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../server" && pwd -P)
source_file="$source_dir/screenframe_server.py"
data_dir="$HOME/.local/state/screenframe-library"
install_dir="$HOME/.local/share/screenframe-library"
unit_dir="$HOME/.config/systemd/user"
unit_name=screenframe-library.service
unit_file="$unit_dir/$unit_name"
token_file="$data_dir/token"
current="$install_dir/current"
port=18745

systemctl --user show-environment >/dev/null
for path in "$data_dir" "$install_dir" "$install_dir/releases"; do
    if [[ -L "$path" || ( -e "$path" && ( ! -d "$path" || ! -O "$path" ) ) ]]; then
        echo "The ScreenFrame library path is not an owner-managed directory: $path" >&2
        exit 1
    fi
done
install -d -m 700 "$data_dir" "$install_dir" "$install_dir/releases"
mkdir -p "$unit_dir"
exec 9>"$install_dir/install.lock"
flock -x 9
work=$(mktemp -d "$install_dir/.deploy.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT

cat >"$work/$unit_name" <<UNIT
[Unit]
Description=ScreenFrame private screenshot library

[Service]
ExecStart=/usr/bin/python3 "%h/.local/share/screenframe-library/current/screenframe_server.py" --data-dir "%h/.local/state/screenframe-library" --token-file "%h/.local/state/screenframe-library/token" --port $port
Restart=on-failure
RestartSec=2
UMask=0077
NoNewPrivileges=yes

[Install]
WantedBy=default.target
UNIT

first_install=false
if [[ -e "$unit_file" || -L "$unit_file" ]]; then
    if [[ -L "$unit_file" ]] || ! cmp -s "$work/$unit_name" "$unit_file"; then
        echo "An unrelated or modified $unit_name already exists." >&2
        exit 1
    fi
else
    first_install=true
fi
old_release=
if [[ -L "$current" ]]; then
    old_release=$(readlink "$current")
    if [[ ! "$old_release" =~ ^releases/[a-f0-9]{64}$ || ! -f "$install_dir/$old_release/screenframe_server.py" ]]; then
        echo "The current ScreenFrame library release is invalid." >&2
        exit 1
    fi
elif [[ -e "$current" ]]; then
    echo "The current ScreenFrame library path is not a release link." >&2
    exit 1
fi
if [[ "$first_install" != true && -z "$old_release" ]]; then
    echo "The existing ScreenFrame library service has no current release." >&2
    exit 1
fi
if [[ "$first_install" == true ]] && ss -H -ltn "sport = :$port" | grep -q .; then
    echo "Port $port is already in use." >&2
    exit 1
fi

if [[ ! -e "$token_file" && ! -L "$token_file" ]]; then
    if [[ "$first_install" != true ]]; then
        echo "The existing ScreenFrame library token is missing." >&2
        exit 1
    fi
    /usr/bin/python3 - "$token_file" <<'PY'
import os
import secrets
import sys
import tempfile

target = sys.argv[1]
with tempfile.NamedTemporaryFile(mode="w", dir=os.path.dirname(target)) as file:
    file.write(secrets.token_urlsafe(32) + "\n")
    file.flush()
    os.fsync(file.fileno())
    os.link(file.name, target)
PY
fi
if [[ ! -f "$token_file" || -L "$token_file" || ! -O "$token_file" || $(stat -c '%a' "$token_file") != 600 ]]; then
    echo "The ScreenFrame library token must be an owner-only regular file (0600)." >&2
    exit 1
fi

release_id=$(sha256sum "$source_file" | cut -d ' ' -f 1)
release="releases/$release_id"
if [[ ! -d "$install_dir/$release" ]]; then
    mkdir "$work/release"
    install -m 600 "$source_file" "$work/release/screenframe_server.py"
    mv -T "$work/release" "$install_dir/$release"
fi
if ! cmp -s "$source_file" "$install_dir/$release/screenframe_server.py"; then
    echo "The ScreenFrame library release contents do not match." >&2
    exit 1
fi

set_release() {
    ln -s "$1" "$work/current"
    mv -Tf "$work/current" "$current"
}

wait_for_health() {
    /usr/bin/python3 - "$port" <<'PY'
import json
import sys
import time
from urllib.error import URLError
from urllib.request import ProxyHandler, build_opener

opener = build_opener(ProxyHandler({}))
for _ in range(20):
    try:
        with opener.open(f"http://127.0.0.1:{sys.argv[1]}/healthz", timeout=0.5) as response:
            if response.status == 200 and json.load(response) == {"status": "ok"}:
                raise SystemExit(0)
    except (OSError, URLError, ValueError):
        pass
    time.sleep(0.25)
raise SystemExit(1)
PY
}

was_active=false
if systemctl --user is-active --quiet "$unit_name"; then
    was_active=true
fi
if [[ "$first_install" == true ]]; then
    set_release "$release"
    install -m 600 "$work/$unit_name" "$unit_file"
    if ! { systemctl --user daemon-reload &&
        systemctl --user enable --now "$unit_name" &&
        systemctl --user is-active --quiet "$unit_name" &&
        wait_for_health; }; then
        systemctl --user disable --now "$unit_name" || true
        rm -f -- "$unit_file" "$current"
        systemctl --user daemon-reload || true
        echo "The ScreenFrame library could not start. Its token and data have been preserved." >&2
        exit 1
    fi
elif [[ "$old_release" != "$release" ]]; then
    set_release "$release"
    if [[ "$was_active" == true ]] && ! { systemctl --user restart "$unit_name" && wait_for_health; }; then
        set_release "$old_release"
        systemctl --user restart "$unit_name" || true
        echo "The ScreenFrame library update failed. The previous release was restored." >&2
        exit 1
    fi
fi

if systemctl --user is-active --quiet "$unit_name"; then
    wait_for_health
    echo "ScreenFrame library is running on 127.0.0.1:$port."
else
    echo "ScreenFrame library files were updated. The service remains stopped."
fi
