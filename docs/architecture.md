# Architecture

## Desktop app

Everything except `main.cpp` is built into the `frameline_core` static library, which the app and the tests link.

| Module | Role |
| --- | --- |
| `main.cpp`, `singleinstance` | Parses the command line and routes window activation to the running desktop instance. Capture commands can run independently. |
| `application` | Owns the windows, tray, shortcuts, library client and hosted library. Runs a capture from request to editor. |
| `capture` | Grabs screens. Uses GDI on Windows, ScreenCaptureKit on macOS (`platform/macos_capture.mm`), the screenshot portal on Wayland and Qt elsewhere. |
| `regionselector` | Shows the frozen frames full-screen and crops the selection, including across monitors with different scale factors. |
| `editor`, `annotations` | Editor window. Annotations are vectors applied to the image in order; undo keeps small pixel patches and replays from the original once they are evicted. |
| `shortcuts` | Global hotkeys: `RegisterHotKey` and a low-level hook for `Win+S` (`platform/windows_shortcuts`), Carbon hotkeys on macOS, the GlobalShortcuts portal on Wayland, XCB grabs on X11. |
| `settings`, `settingswindow` | `~/.frameline/settings.json`, library connection codes and the settings dialog. |
| `platform/autostart` | Registers `--tray` at user login through Windows Run, a macOS LaunchAgent or XDG autostart on Linux. |
| `libraryclient`, `librarywindow` | Upload queue, connection to the library and library browser. |
| `libraryserver`, `tailscale` | Embedded library storage and API, plus detection of the private network for sharing. |
| `theme` | Design tokens: light and dark palettes that follow the system, the type scale, the shared style sheet and painting helpers (shadows, viewfinder brackets, key caps). |
| `controls` | Self-painted controls used by every window: buttons, toggle, segmented control, floating islands, toasts, banners and empty states. |
| `pngexport` | PNG writing and the save dialog. |

`tools/shots.cpp` builds `frameline_shots`, which renders every window and state to PNG for visual review: `frameline_shots <dir> [port token]` (pass a running library's port and token to include the grid and viewer), and `QT_QPA_PLATFORM=offscreen frameline_shots <dir> --region` for the selection overlay. The README demo is recorded with `--demo-editor` and, offscreen, `--demo-region`; each run writes numbered frames and a `frames.txt` with the pointer position per frame, for compositing into `docs/media/demo.gif`. `FRAMELINE_THEME=light` or `dark` overrides the system appearance.

Platform code stays behind `#if defined(Q_OS_…)` in `capture`, `shortcuts` and `regionselector`, or in `src/platform`.

## Capturing

`FramelineApplication::beginCapture` hides Frameline's windows, waits for the compositor where needed, and asks `ScreenCapture` for frames. A full capture goes straight to the editor; a region capture goes through `RegionSelector` first. The editor holds the image until it is closed or replaced. At that point, if archiving is on, the edited pixels are handed to the library client.

`frameline capture … --no-preview` skips the editor and queues the capture immediately.

## Library queue

The client writes each capture (PNG, thumbnail, `metadata.json`) to a staging directory and then renames it into the queue, so a queued entry is always complete. Queues live in `~/.frameline/queues/<key>`. The key hashes the transport, destination, port and token so captures queued for one library stay with that library.

Several Frameline processes (tray, CLI, GNOME hotkey) may share a queue. Each entry is uploaded under a lock file, and before queueing or uploading the client rereads the saved settings so that a process started with old settings stops when archiving is turned off or the destination changes. Uploads use the capture's UUID, so retrying after a failure is safe: the server answers an identical retry with `204`.

For SSH destinations the client runs `ssh -L` with `BatchMode=yes` and `StrictHostKeyChecking=yes` and waits for a marker line before sending requests.

For Tailscale destinations, the client verifies that the address in the code is reachable through Tailscale. It relays HTTP through a local listener whose outbound connection binds to this computer's Tailscale address. If the private route is unavailable, uploads stay queued.

## Library servers

The embedded library runs in the desktop process, including when Frameline starts in the tray at login. The application saves the hosting settings and starts the server again on subsequent launches. It stores each capture and its metadata under `~/.frameline/library` by default, separate from the standalone server's SQLite database.

The server listens on loopback for the local client. When sharing is enabled and Tailscale is connected, it also listens on the computer's verified Tailscale address. The connection code carries this address, the server port and a generated access token. Devices that receive the code need access to the same Tailscale network.

`server/frameline_server.py` provides the same library API for separately managed deployments. It uses only the Python standard library, listens on loopback and checks the access token and upload limits. It stores images as files and metadata in SQLite. See the [standalone server instructions](../server/README.md).
