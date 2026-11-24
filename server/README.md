# Standalone Frameline library server

To share screenshots from the desktop app, use **Settings → Sync across devices → Host here**. This guide covers a separately managed Python server, including running it when the desktop app is closed. Each server keeps its own library files. [Set up the embedded library](../README.md#sync-across-devices)

## Start a library

On Ubuntu with systemd, run `bash scripts/install-ubuntu-library.sh` from the repository root as your regular user. The service stores screenshots and the access token under `~/.local/state/screenframe-library`. Frameline was called ScreenFrame; the service, its folders and the `X-ScreenFrame-*` headers keep that name so existing libraries keep working. If the server must run while you are signed out, enable user lingering with `loginctl enable-linger "$USER"`.

To run the server directly, use Python 3.10 or newer. On a fresh setup, create a private data directory and generate a token once.

```sh
umask 077
library="$HOME/.local/state/screenframe-library"
mkdir -p "$library"
(set -C; python3 -c 'import secrets; print(secrets.token_urlsafe(32))' > "$library/token")
```

Reuse the token whenever you restart the server.

```sh
python3 server/frameline_server.py --data-dir "$library" --token-file "$library/token"
```

The server listens on `127.0.0.1:18745` by default. Use `--port` to change the loopback port. Keep the token file at mode 0600 and the data directory at mode 0700. A token contains 32–256 URL-safe ASCII characters.

## Connect a device

Open **Settings → Sync across devices → Advanced** and choose **This computer** on the server host. On a different device, choose **Over SSH**. Set up your SSH key and host alias in your OpenSSH client first. For example, add an entry to `~/.ssh/config` and substitute your server's host and SSH user.

```sshconfig
Host screenshots-server
    HostName server.example.net
    User library-user
```

Verify the host key when you connect for the first time. `ssh -o BatchMode=yes screenshots-server true` should then succeed without a prompt. The SSH server must allow local port forwarding. In Frameline, enter the same host alias, the library port (18745 by default), the token, and a device name. Select **Test connection** before saving. Enable **Upload screenshots from this device** wherever you want automatic uploads.

The shared bearer token grants read and write access to the library. Give it only to devices you trust. Screenshots stay on disk indefinitely. The server has no automatic retention policy or total storage quota.

## HTTP API

All library requests use `Authorization: Bearer <token>`. The loopback health endpoint `GET /healthz` returns `{"status":"ok"}`.

- `PUT /v1/screenshots/<id>/image` accepts an `image/png` body with `Content-Length`. Use a lowercase UUID as `id`. The required headers are `X-ScreenFrame-Captured-At` (RFC 3339), `X-ScreenFrame-Width`, `X-ScreenFrame-Height`, `X-ScreenFrame-Device` (1–64 printable ASCII characters), and `X-ScreenFrame-Kind` (`full` or `region`). Originals are limited to 64 MiB and 100 megapixels.
- `PUT /v1/screenshots/<id>/thumbnail` accepts `image/png` or `image/jpeg` with `Content-Length` after the original is stored. Thumbnails are limited to 2 MiB and 1024 pixels per side.
- An initial successful upload returns `201`. An identical retry returns `204`. Uploading different bytes or metadata with the same ID and part returns `409`.
- `GET /v1/screenshots?limit=50&cursor=<nextCursor>` lists captures newest first. Omit `cursor` on the first request. The response contains `items` and `nextCursor`, which is null at the end. Each item has `id`, `capturedAt`, `width`, `height`, `device`, `kind`, and `hasThumbnail`. The allowed limit is 1–100.
- `GET /v1/screenshots/<id>/image` or `GET /v1/screenshots/<id>/thumbnail` returns the image bytes. A part that has yet to be uploaded returns `404`.

Run the HTTP and storage tests from the repository root with `python3 -B -m unittest discover -s server`.
