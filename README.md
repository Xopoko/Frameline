# ScreenFrame

ScreenFrame is a native screenshot utility built with C++ and Qt Widgets. Capture from the app, the system tray, a global shortcut, or the command line.

## Install

- **Windows 10 (1809 or later) and Windows 11, x64.** Extract the ZIP and open `bin/ScreenFrameGUI.exe`. The CLI is `bin/screenframe.exe`.
- **macOS 14.4+.** Move `ScreenFrame.app` to Applications. Allow ScreenFrame in **System Settings → Privacy & Security → Screen & System Audio Recording**, then reopen it.
- **Ubuntu 24.04 amd64.** Run `sudo apt install ./screenframe_0.2.0_amd64.deb`, then launch ScreenFrame from the application menu. X11 and GNOME Wayland use different capture backends.

Capture, edit, copy, and save work on a fresh install without a server or account. See the [release guide](docs/release.md) for preview package and platform support details.

## Capture and edit

| Action | Windows and Ubuntu | macOS |
| --- | --- | --- |
| Capture the screen | `Ctrl+Alt+Shift+1` | `⌘⌥⇧1` |
| Capture a region | `Ctrl+Alt+Shift+2` | `⌘⌥⇧2` |

Keep ScreenFrame running to use its global shortcuts. To use the system region shortcut, enable the option in **Settings** and reopen ScreenFrame. This uses `Win+S` on Windows, `⌘⇧5` on macOS, or `Super+S` on Linux. On macOS, first turn off **Screenshot and recording options** in **System Settings → Keyboard → Keyboard Shortcuts → Screenshots**. On Linux, desktop settings may own the key.

Full-screen capture takes the monitor under the pointer on Windows, macOS, and X11. For a region capture, drag over the frozen screen and release to open the crop in the editor. Press `Esc` to cancel.

In the editor, use Arrow (`A`), Pen (`P`), Rectangle (`R`), Blur (`B`), or Redact (`X`). Choose a color and line width in the toolbar. Save and copy apply your edits to the exported image. Use opaque redaction for sensitive details.

## Shared screenshot library

Open **Library** from the editor or tray, or run `screenframe library`. Choose **Set up library** on the first visit. Select an image to view it at full resolution or save a PNG copy.

Enable **Automatically archive screenshots from this device** to upload the final image when you close the editor, replace the capture, or quit. Copy and Save PNG leave the capture open for more edits. `--no-preview` queues the capture immediately. If the server is unavailable, ScreenFrame keeps the queued PNG locally and retries. Connecting a library without enabling archiving gives the device browsing access without uploading new captures.

To host the library on Ubuntu, run `bash scripts/install-ubuntu-library.sh` as your user. It creates a user systemd service and stores data and the access token under `~/.local/state/screenframe-library`. The [server guide](server/README.md) also covers running it directly.

In **Settings → Shared library**, choose **Library on this computer** on the server host, or **Library over SSH** on another device. For SSH, enter a host or alias from your OpenSSH configuration. It must connect noninteractively with host-key verification. Enter the server port, access token, and a device label, then choose **Test Connection** and **Save Settings**. Keep the token private. Every device with the token can read the library.

Existing `~/.screenframe/library.json` setups keep their settings and pending uploads on upgrade. Disabling automatic archiving pauses queued uploads. Changing the destination leaves queued files with their original connection. Reconnect to that same destination and enable archiving to resume its queue.

### Ubuntu and Wayland

Wayland can prompt for screen capture permission. A full-screen capture contains the desktop returned by the screenshot portal. For a region capture, ScreenFrame displays the frozen desktop image on one monitor. GNOME may also save the original screenshot in Pictures.

On GNOME, the system can run `screenframe capture region` for `Super+S` even when ScreenFrame is closed. The optional setup tool replaces the standard GNOME binding and can restore it later. Run it as the signed-in desktop user.

```sh
python3 /usr/share/screenframe/configure-gnome-shortcut.py install
python3 /usr/share/screenframe/configure-gnome-shortcut.py status
python3 /usr/share/screenframe/configure-gnome-shortcut.py remove
```

If the global shortcuts portal is unavailable, set `screenframe capture full` and `screenframe capture region` as custom commands in your desktop's keyboard settings. Use any free key combinations, such as the defaults in the table.

## Command line

```sh
screenframe --help
screenframe --tray
screenframe library
screenframe capture full
screenframe capture region --output area.png
screenframe capture full --output screenshot.png --no-preview
```

Use `screenframe.exe` on Windows and `/Applications/ScreenFrame.app/Contents/MacOS/ScreenFrame` on macOS. Capture requires a graphical session. `--output` saves the original capture immediately. Save any later edits from the editor. `--no-preview` exits after writing the PNG.

The exit code is `0` on success, `1` on error, or `2` if you cancel the capture.

## Build from source

Use CMake 3.21.1 or newer, a C++20 compiler, and Qt 6.4 or newer with Widgets. Use MSVC with Qt 6.8 or newer on Windows, and Xcode with the official Qt macOS SDK on macOS. Set `-DCMAKE_PREFIX_PATH=/path/to/Qt` if CMake needs help finding Qt.

On Ubuntu, install the build dependencies.

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev libx11-dev libxcb1-dev qt6-wayland
```

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Run `cmake --install build --prefix <absolute-path>` to stage the app and its Qt dependencies on Windows and macOS. On Ubuntu, run `cpack --config build/CPackConfig.cmake -B dist` to build a DEB.

See [Contributing](CONTRIBUTING.md), [Architecture](docs/architecture.md), and [Security](SECURITY.md). ScreenFrame source is available under the [MIT license](LICENSE). Qt and other bundled components have their own licenses.
