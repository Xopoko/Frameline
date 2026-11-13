# Contributing

Build and test as described in the [README](README.md#building). Run the server tests too if you touch the library.

When changing capture or shortcuts, say which OS you tested on, and for Linux whether it was X11 or Wayland. Screenshots in issues and pull requests should use sample content, not your own desktop, settings, tokens or logs with personal paths.

Capture must keep working with no library configured. For upload changes, also try an unreachable server, a restart with captures still queued, and a destination change while a capture is queued. Platform-specific calls belong in `src/platform` or behind `Q_OS_…` checks; see [architecture](docs/architecture.md).
