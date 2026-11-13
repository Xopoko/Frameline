# Architecture

The desktop application is C++20 and Qt Widgets. The editor and region selector consume images and display geometry from the platform capture backend. Native shortcuts are registered independently of the capture and library features. The default region preset leaves operating-system shortcuts available.

ScreenFrame starts in local mode on a fresh profile. Capture, edit, save, and copy work without a library. Settings store the optional connection and a separate automatic archiving preference. The library client connects directly to loopback or starts an authenticated OpenSSH forward using a configured host alias. Connection tests read the screenshot list. They leave the upload queue untouched.

The editor holds a capture until it closes or a new capture replaces it. On a normal exit, ScreenFrame archives the final edited pixels if automatic archiving is on. CLI capture with `--no-preview` queues the image immediately. Changing the destination or archiving preference while a capture is open keeps that capture local.

The client writes the PNG, thumbnail, and metadata to a temporary directory and then moves the directory into the upload queue. Queue folders are scoped to the connection type, SSH alias, port, and access token. The client checks the saved preference and destination before adding a capture or starting an upload, including when several CLI and desktop processes are running. An existing pre-0.2 queue is used only for its legacy connection. The uploader retries the same ID after connection failures. The server treats an identical retry as success.

The optional library server uses Python's standard library, a private data directory, SQLite metadata, and files for the original PNGs and thumbnails. It binds to loopback and authenticates access to the library API. Linux installation uses a user systemd service. Other hosts can run the server directly. See the [server guide](../server/README.md).
