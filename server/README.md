# ScreenFrame library server

Run with Python 3.10 or newer.

```sh
python3 server/screenframe_server.py --data-dir "$HOME/.local/share/screenframe/library" \
  --token-file "$HOME/.config/screenframe/library.token"
```

The server listens on `127.0.0.1:18745` by default. Clients on other devices connect over an authenticated SSH tunnel. Set `--port` if you need a different loopback port. On Ubuntu, the service user needs a private token file and data directory. A token contains 32–256 URL-safe ASCII characters. Use mode 0600 for the token file and 0700 for the data directory.

## HTTP API

All library requests use `Authorization: Bearer <token>`. The loopback health endpoint `GET /healthz` returns `{"status":"ok"}`.

- `PUT /v1/screenshots/<id>/image` accepts an `image/png` body with `Content-Length`. Use a lowercase UUID as `id`. The required headers are `X-ScreenFrame-Captured-At` (RFC 3339), `X-ScreenFrame-Width`, `X-ScreenFrame-Height`, `X-ScreenFrame-Device` (1–64 printable ASCII characters), and `X-ScreenFrame-Kind` (`full` or `region`). Originals are limited to 64 MiB and 100 megapixels.
- `PUT /v1/screenshots/<id>/thumbnail` accepts `image/png` or `image/jpeg` with `Content-Length` after the original is stored. Thumbnails are limited to 2 MiB and 1024 pixels per side.
- An initial successful upload returns `201`. An identical retry returns `204`. Uploading different bytes or metadata with the same ID and part returns `409`.
- `GET /v1/screenshots?limit=50&cursor=<nextCursor>` lists captures newest first. Omit `cursor` on the first request. The response contains `items` and `nextCursor`, which is null at the end. Each item has `id`, `capturedAt`, `width`, `height`, `device`, `kind`, and `hasThumbnail`. The allowed limit is 1–100.
- `GET /v1/screenshots/<id>/image` or `GET /v1/screenshots/<id>/thumbnail` returns the image bytes. A part that has yet to be uploaded returns `404`.

Run the HTTP and storage tests from the repository root with `python3 -B -m unittest discover -s server`.
