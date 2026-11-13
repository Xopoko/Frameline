# Contributing to ScreenFrame

Use the build commands in the [README](README.md). Build with `BUILD_TESTING=ON`, run CTest, and run the Python server tests for any library change.

For capture or keyboard changes, include the operating system, display server on Linux, and whether you tested a physical desktop. For interface changes, use sample images in screenshots. Omit private desktops, configuration files, tokens, and logs containing personal paths.

Changes should preserve normal capture with the shared library disabled. When touching uploads, test an unavailable server, a retry after restart, and a configuration change with a pending capture. Keep platform-specific calls in the platform backend. See the [architecture notes](docs/architecture.md).
