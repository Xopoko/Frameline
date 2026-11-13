# Architecture

## Desktop app

Everything except `main.cpp` is built into the `screenframe_core` static library, which the app and the tests link.

| Module | Role |
| --- | --- |
| `main.cpp` | Parses the command line into `LaunchOptions`. |
| `application` | Owns the windows, tray, shortcuts and library client, and runs a capture from request to editor. |
| `capture` | Grabs screens. Uses GDI on Windows, ScreenCaptureKit on macOS (`platform/macos_capture.mm`), the screenshot portal on Wayland and Qt elsewhere. |
| `regionselector` | Shows the frozen frames full-screen and crops the selection, including across monitors with different scale factors. |
| `editor`, `annotations` | Editor window. Annotations are vectors applied to the image in order; undo keeps small pixel patches and replays from the original once they are evicted. |
| `shortcuts` | Global hotkeys: `RegisterHotKey` and a low-level hook for `Win+S` (`platform/windows_shortcuts`), Carbon hotkeys on macOS, the GlobalShortcuts portal on Wayland, XCB grabs on X11. |
| `settings`, `settingswindow` | `~/.screenframe/settings.json` and the dialog that edits it. |
| `libraryclient`, `librarywindow` | Upload queue, HTTP client and SSH tunnel; the library browser. |
| `theme` | Design tokens: light and dark palettes that follow the system, the type scale, the shared style sheet and painting helpers (shadows, viewfinder brackets, key caps). |
| `controls` | Self-painted controls used by every window: buttons, toggle, segmented control, floating islands, toasts, banners and empty states. |
| `pngexport` | PNG writing and the save dialog. |

`tools/shots.cpp` builds `screenframe_shots`, which renders every window and state to PNG for visual review: `screenframe_shots <dir> [port token]` (pass a running library's port and token to include the grid and viewer), and `QT_QPA_PLATFORM=offscreen screenframe_shots <dir> --region` for the selection overlay. The README demo is recorded with `--demo-editor` and, offscreen, `--demo-region`; each run writes numbered frames and a `frames.txt` with the pointer position per frame, for compositing into `docs/media/demo.gif`. `SCREENFRAME_THEME=light` or `dark` overrides the system appearance.

Platform code stays behind `#if defined(Q_OS_…)` in `capture`, `shortcuts` and `regionselector`, or in `src/platform`.

## Capturing

`ScreenFrameApplication::beginCapture` hides ScreenFrame's windows, waits for the compositor where needed, and asks `ScreenCapture` for frames. A full capture goes straight to the editor; a region capture goes through `RegionSelector` first. The editor holds the image until it is closed or replaced. At that point, if archiving is on, the edited pixels are handed to the library client.

`screenframe capture … --no-preview` skips the editor and queues the capture immediately.

## Library queue

The client writes each capture (PNG, thumbnail, `metadata.json`) to a staging directory and then renames it into the queue, so a queued entry is always complete. Queues live in `~/.screenframe/queues/<key>`, where the key is a hash of transport, SSH host, port and token. Changing any of these starts a new queue and never sends old captures to a new server.

Several ScreenFrame processes (tray, CLI, GNOME hotkey) may share a queue. Each entry is uploaded under a lock file, and before queueing or uploading the client rereads the saved settings so that a process started with old settings stops when archiving is turned off or the destination changes. Uploads use the capture's UUID, so retrying after a failure is safe: the server answers an identical retry with `204`.

For SSH destinations the client runs `ssh -L` with `BatchMode=yes` and `StrictHostKeyChecking=yes` and waits for a marker line before sending requests.

## Library server

`server/screenframe_server.py` uses only the Python standard library. It listens on loopback, checks a bearer token, stores originals and thumbnails as files and their metadata in SQLite, and checks every upload's image header and size before storing it. On Ubuntu it runs as a user systemd service; see [server/README.md](../server/README.md).
