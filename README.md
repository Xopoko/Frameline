# ScreenFrame

ScreenFrame is a native screenshot utility built with C++ and Qt Widgets. Capture from the app, the system tray, a global shortcut, or the command line.

## Install

- **Windows 10/11 x64.** Extract the ZIP and open `bin/ScreenFrameGUI.exe`. The CLI is `bin/screenframe.exe`.
- **macOS 14.4+, Apple Silicon.** Open the DMG and move `ScreenFrame.app` to Applications. Allow ScreenFrame in **System Settings → Privacy & Security → Screen & System Audio Recording**, then reopen it. The packaged app is ad hoc signed.
- **Ubuntu 24.04+ amd64.** Run `sudo apt install ./screenframe_0.1.2_amd64.deb`, then launch ScreenFrame from the application menu.

## Capture and edit

| Action | Windows and Ubuntu | macOS |
| --- | --- | --- |
| Capture the screen | `Ctrl+Alt+Shift+1` | `⌘⌥⇧1` |
| Capture a region | `Win+S` (`Super+S` on Ubuntu) | `⌘⇧5` |

Keep ScreenFrame running to use its global shortcuts. On Windows, `Win+S` opens ScreenFrame's region selector in place of Windows Search while the app is running. On macOS, open **System Settings → Keyboard → Keyboard Shortcuts → Screenshots**, turn off **Screenshot and recording options**, and reopen ScreenFrame. This frees `⌘⇧5` for ScreenFrame.

Full-screen capture takes the monitor under the pointer on Windows, macOS, and X11. For a region capture, drag over the frozen screen and release to open the crop in the editor. Press `Esc` to cancel.

In the editor, use Arrow (`A`), Pen (`P`), Rectangle (`R`), Blur (`B`), or Redact (`X`). Choose a color and line width in the toolbar. Save and copy apply your edits to the exported image. Use opaque redaction for sensitive details.

### Ubuntu and Wayland

Wayland can prompt for screen capture permission. A full-screen capture contains the desktop returned by the screenshot portal. For a region capture, ScreenFrame displays the frozen desktop image on one monitor. GNOME may also save the original screenshot in Pictures.

On GNOME, the system can run `screenframe capture region` for `Super+S` even when ScreenFrame is closed. The included setup tool replaces the standard GNOME binding for that combination and can restore it later. Run it as the signed-in desktop user.

```sh
python3 /usr/share/screenframe/configure-gnome-shortcut.py install
python3 /usr/share/screenframe/configure-gnome-shortcut.py remove
```

On other desktops, free `Super+S` in the keyboard settings and assign it to `screenframe capture region` if ScreenFrame cannot register it. You can also assign `Ctrl+Alt+Shift+1` to `screenframe capture full` on systems without the global shortcuts portal.

## Command line

```sh
screenframe --help
screenframe --tray
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

For the arm64 DMG, configure with `-DCMAKE_OSX_ARCHITECTURES=arm64` and run `bash scripts/package-macos.sh build dist/ScreenFrame-0.1.2-macOS-arm64.dmg` after building.
