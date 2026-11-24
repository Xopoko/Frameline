#!/usr/bin/env python3
"""Private, loopback-only FreezeCut screenshot library."""

import argparse
import base64
import binascii
from contextlib import closing
from datetime import datetime, timezone
from hashlib import sha256
from hmac import compare_digest
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3
import stat
import struct
import tempfile
from threading import BoundedSemaphore
from time import monotonic, time
from urllib.parse import parse_qs, urlsplit


DEFAULT_PORT = 18745
MAX_IMAGE = 64 * 1024 * 1024
MAX_THUMBNAIL = 2 * 1024 * 1024
MAX_SIDE = 32768
MAX_PIXELS = 100_000_000
MAX_THUMBNAIL_SIDE = 1024
IO_TIMEOUT = 10
UPLOAD_TIMEOUT = 60
CHUNK = 128 * 1024
MAX_REQUESTS = 8
IMAGE_ROOT = "/v1/screenshots"
UUID_PATTERN = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")
TOKEN_PATTERN = re.compile(rb"[A-Za-z0-9_-]{32,256}")
TIMESTAMP_PATTERN = re.compile(
    r"([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2})"
    r"(?:\.([0-9]{1,9}))?(Z|[+-][0-9]{2}:[0-9]{2})"
)
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
PNG_END = bytes.fromhex("0000000049454e44ae426082")
PNG_DEPTHS = {0: {1, 2, 4, 8, 16}, 2: {8, 16}, 3: {1, 2, 4, 8}, 4: {8, 16}, 6: {8, 16}}
JPEG_FRAME_MARKERS = {
    0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF
}


class RequestError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def timestamp(value):
    match = TIMESTAMP_PATTERN.fullmatch(value)
    if not match:
        raise ValueError("Expected RFC3339 timestamp")
    seconds, fraction, offset = match.groups()
    if fraction:
        # Python 3.10 accepts only three or six fractional digits.
        seconds += "." + fraction[:6].ljust(6, "0")
    return datetime.fromisoformat(seconds + ("+00:00" if offset == "Z" else offset)).astimezone(
        timezone.utc
    ).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def cursor_for(row):
    payload = f"{row['captured_at']}/{row['id']}".encode("ascii")
    return base64.urlsafe_b64encode(payload).decode("ascii").rstrip("=")


def parse_cursor(value):
    try:
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", value):
            raise ValueError("Invalid base64")
        padded = value + "=" * (-len(value) % 4)
        captured_at, image_id = base64.b64decode(padded, altchars=b"-_", validate=True).decode(
            "ascii"
        ).split("/")
        if timestamp(captured_at) != captured_at or not UUID_PATTERN.fullmatch(image_id):
            raise ValueError("Invalid cursor value")
        return captured_at, image_id
    except (ValueError, OverflowError, binascii.Error) as error:
        raise RequestError(HTTPStatus.BAD_REQUEST, "Invalid cursor") from error


def png_size(file, length):
    file.seek(0)
    header = file.read(33)
    if length < 45 or header[:16] != PNG_SIGNATURE + b"\0\0\0\rIHDR":
        return None
    if binascii.crc32(header[12:29]) != int.from_bytes(header[29:33], "big"):
        return None
    width, height, depth, color, compression, filtering, interlace = struct.unpack_from(">IIBBBBB", header, 16)
    if depth not in PNG_DEPTHS.get(color, ()) or compression or filtering or interlace > 1:
        return None
    has_pixels = False
    for _ in range(16_384):
        if file.tell() + 12 > length:
            return None
        chunk = file.read(8)
        if chunk[4:] == b"IEND":
            if has_pixels and file.tell() + 4 == length and chunk + file.read(4) == PNG_END:
                return width, height
            return None
        size = int.from_bytes(chunk[:4], "big")
        if size > length - file.tell() - 4:
            return None
        has_pixels |= chunk[4:] == b"IDAT" and size > 0
        file.seek(size + 4, os.SEEK_CUR)
    return None


def jpeg_size(file, length):
    file.seek(-min(length, 2), os.SEEK_END)
    if file.read(2) != b"\xff\xd9":
        return None
    file.seek(0)
    if file.read(2) != b"\xff\xd8":
        return None
    while file.tell() + 4 <= length and file.read(1) == b"\xff":
        marker = file.read(1)
        while marker == b"\xff":
            marker = file.read(1)
        if not marker or marker in (b"\0", b"\xda", b"\xd9"):
            break
        segment = int.from_bytes(file.read(2), "big")
        if segment < 2 or file.tell() + segment - 2 > length:
            break
        if marker[0] in JPEG_FRAME_MARKERS and segment >= 8:
            precision, height, width = struct.unpack(">BHH", file.read(5))
            if precision == 8:
                return width, height
            break
        file.seek(segment - 2, os.SEEK_CUR)
    return None


def read_token(path):
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_BINARY", 0)
    with os.fdopen(os.open(path, flags | getattr(os, "O_NONBLOCK", 0)), "rb") as file:
        info = os.fstat(file.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise ValueError("Token must be a regular file")
        if os.name == "posix" and (info.st_uid != os.getuid() or info.st_mode & 0o077):
            raise ValueError("Token file must be owned by the service user and private")
        token = file.read(259).removesuffix(b"\n")
        if not TOKEN_PATTERN.fullmatch(token):
            raise ValueError("Token must contain 32–256 URL-safe ASCII characters")
    return token


class Library:
    def __init__(self, directory):
        directory = Path(directory).expanduser().absolute()
        directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        info = directory.lstat()
        if not stat.S_ISDIR(info.st_mode):
            raise ValueError("Data directory must be a directory")
        if os.name == "posix" and (info.st_uid != os.getuid() or info.st_mode & 0o077):
            raise ValueError("Data directory must be owned by the service user and private")
        self.directory = directory
        self.db = self.directory / "library.sqlite3"
        self.cleanup_abandoned_uploads()
        with closing(self.connect()) as db:
            db.execute("PRAGMA journal_mode=WAL")
            db.executescript("""
                CREATE TABLE IF NOT EXISTS screenshots (
                    id TEXT PRIMARY KEY,
                    captured_at TEXT NOT NULL,
                    width INTEGER NOT NULL,
                    height INTEGER NOT NULL,
                    device TEXT NOT NULL,
                    kind TEXT NOT NULL,
                    image_sha TEXT NOT NULL,
                    thumb_sha TEXT,
                    thumb_type TEXT
                );
                CREATE INDEX IF NOT EXISTS screenshots_capture_order
                    ON screenshots(captured_at DESC, id DESC);
            """)

    def cleanup_abandoned_uploads(self):
        # A live upload times out in a minute. Older staging files can remain after a crash.
        cutoff = time() - 86_400
        for file in self.directory.glob(".upload-*"):
            try:
                info = file.lstat()
                if stat.S_ISREG(info.st_mode) and info.st_mtime < cutoff:
                    file.unlink()
            except FileNotFoundError:
                continue

    def connect(self):
        db = sqlite3.connect(self.db, timeout=5, isolation_level=None)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA synchronous=FULL")
        return db

    def path_for(self, image_id, part):
        return self.directory / f"{image_id}.{part}"

    def sync_directory(self):
        if os.name == "posix":
            fd = os.open(self.directory, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)

    def add(self, image_id, part, staged, digest, media_type, metadata):
        with closing(self.connect()) as db, db:
            # Keep concurrent retries serialized until the file and index are durable.
            db.execute("BEGIN IMMEDIATE")
            previous = db.execute(
                "SELECT * FROM screenshots WHERE id = ?", (image_id,)
            ).fetchone()
            destination = self.path_for(image_id, part)
            if part == "image":
                if previous is not None:
                    existing = tuple(previous[key] for key in (
                        "captured_at", "width", "height", "device", "kind", "image_sha"
                    ))
                    if existing != (*metadata, digest):
                        raise RequestError(HTTPStatus.CONFLICT, "Screenshot ID already used")
                    if not destination.exists():
                        os.replace(staged, destination)
                        self.sync_directory()
                    return HTTPStatus.NO_CONTENT
                db.execute(
                    "INSERT INTO screenshots (id, captured_at, width, height, device, kind, image_sha)"
                    " VALUES (?, ?, ?, ?, ?, ?, ?)",
                    (image_id, *metadata, digest),
                )
            else:
                if previous is None:
                    raise RequestError(HTTPStatus.NOT_FOUND, "Screenshot does not exist")
                if previous["thumb_sha"] is not None:
                    if previous["thumb_sha"] != digest or previous["thumb_type"] != media_type:
                        raise RequestError(HTTPStatus.CONFLICT, "Thumbnail already exists")
                    if not destination.exists():
                        os.replace(staged, destination)
                        self.sync_directory()
                    return HTTPStatus.NO_CONTENT
                db.execute(
                    "UPDATE screenshots SET thumb_sha = ?, thumb_type = ? WHERE id = ?",
                    (digest, media_type, image_id),
                )
            os.replace(staged, destination)
            self.sync_directory()
        return HTTPStatus.CREATED

    def items(self, limit, cursor):
        query = "SELECT * FROM screenshots"
        parameters = []
        if cursor is not None:
            query += " WHERE (captured_at, id) < (?, ?)"
            parameters.extend(cursor)
        query += " ORDER BY captured_at DESC, id DESC LIMIT ?"
        parameters.append(limit + 1)
        with closing(self.connect()) as db:
            rows = db.execute(query, parameters).fetchall()
        items = [
            {
                "id": row["id"],
                "capturedAt": row["captured_at"],
                "width": row["width"],
                "height": row["height"],
                "device": row["device"],
                "kind": row["kind"],
                "hasThumbnail": row["thumb_sha"] is not None,
            }
            for row in rows[:limit]
        ]
        return {"items": items, "nextCursor": cursor_for(rows[limit - 1]) if len(rows) > limit else None}

    def open(self, image_id, part):
        with closing(self.connect()) as db:
            row = db.execute(
                "SELECT thumb_type FROM screenshots WHERE id = ?", (image_id,)
            ).fetchone()
        if row is None or (part == "thumbnail" and row["thumb_type"] is None):
            raise RequestError(HTTPStatus.NOT_FOUND, "Image does not exist")
        media_type = "image/png" if part == "image" else row["thumb_type"]
        try:
            return self.path_for(image_id, part).open("rb"), media_type
        except FileNotFoundError as error:
            raise RequestError(HTTPStatus.NOT_FOUND, "Image does not exist") from error


class Server(ThreadingHTTPServer):
    request_queue_size = 32

    def __init__(self, library, token, port):
        self.library = library
        self.token = b"Bearer " + token
        self.available = BoundedSemaphore(MAX_REQUESTS)
        super().__init__(("127.0.0.1", port), Handler)

    def process_request(self, request, client_address):
        if not self.available.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.available.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.available.release()


class Handler(BaseHTTPRequestHandler):
    server_version = "FreezeCut"
    sys_version = ""

    def setup(self):
        self.request.settimeout(IO_TIMEOUT)
        super().setup()

    def log_message(self, *_args):
        pass

    def do_GET(self):
        self.dispatch(self.get)

    def do_PUT(self):
        self.dispatch(self.put)

    def dispatch(self, operation):
        try:
            try:
                url = urlsplit(self.path)
            except ValueError as error:
                raise RequestError(HTTPStatus.BAD_REQUEST, "Invalid request URL") from error
            if self.command == "GET" and url.path == "/healthz":
                self.send_json(HTTPStatus.OK, {"status": "ok"})
                return
            if url.path != IMAGE_ROOT and not url.path.startswith(IMAGE_ROOT + "/"):
                raise RequestError(HTTPStatus.NOT_FOUND, "Endpoint does not exist")
            authorization = self.headers.get_all("Authorization", [])
            if len(authorization) != 1 or not compare_digest(
                authorization[0].encode("latin-1"), self.server.token
            ):
                raise RequestError(HTTPStatus.UNAUTHORIZED, "Authorization required")
            operation(url)
        except RequestError as error:
            self.send_json(error.status, {"error": str(error)})
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            self.close_connection = True
        except (OSError, sqlite3.Error) as error:
            print(f"FreezeCut storage error: {type(error).__name__}", flush=True)
            self.send_json(HTTPStatus.INTERNAL_SERVER_ERROR, {"error": "Library storage failed"})

    def get(self, url):
        if url.path == IMAGE_ROOT:
            try:
                params = parse_qs(url.query, keep_blank_values=True, max_num_fields=2)
            except ValueError as error:
                raise RequestError(HTTPStatus.BAD_REQUEST, "Invalid list parameters") from error
            if any(key not in {"limit", "cursor"} or len(values) != 1 for key, values in params.items()):
                raise RequestError(HTTPStatus.BAD_REQUEST, "Invalid list parameters")
            raw_limit = params.get("limit", ["50"])[0]
            if not re.fullmatch(r"[0-9]{1,3}", raw_limit) or not 1 <= int(raw_limit) <= 100:
                raise RequestError(HTTPStatus.BAD_REQUEST, "Limit must be between 1 and 100")
            cursor = parse_cursor(params["cursor"][0]) if "cursor" in params else None
            self.send_json(HTTPStatus.OK, self.server.library.items(int(raw_limit), cursor))
            return

        image_id, part = self.image_path(url.path)
        file, media_type = self.server.library.open(image_id, part)
        with file:
            self.send_headers(HTTPStatus.OK, os.fstat(file.fileno()).st_size, media_type)
            shutil.copyfileobj(file, self.wfile, length=CHUNK)

    def put(self, url):
        image_id, part = self.image_path(url.path)
        types = {"image/png"} if part == "image" else {"image/png", "image/jpeg"}
        media_type = self.header("Content-Type")
        if media_type not in types:
            raise RequestError(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, "Unsupported image type")
        if self.headers.get_all("Transfer-Encoding"):
            raise RequestError(HTTPStatus.BAD_REQUEST, "Transfer-Encoding is unsupported")
        raw_length = self.header("Content-Length")
        if not re.fullmatch(r"[0-9]{1,20}", raw_length):
            raise RequestError(HTTPStatus.LENGTH_REQUIRED, "Content-Length required")
        length = int(raw_length)
        limit = MAX_IMAGE if part == "image" else MAX_THUMBNAIL
        if not 0 < length <= limit:
            raise RequestError(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Image exceeds allowed size")

        metadata = self.metadata() if part == "image" else None
        staged = self.stage(length, part, media_type, metadata)
        try:
            path, digest = staged
            status = self.server.library.add(image_id, part, path, digest, media_type, metadata)
        finally:
            staged[0].unlink(missing_ok=True)
        self.send_headers(status, 0)

    def image_path(self, path):
        parts = path.removeprefix(IMAGE_ROOT + "/").split("/")
        if len(parts) != 2 or parts[1] not in {"image", "thumbnail"}:
            raise RequestError(HTTPStatus.NOT_FOUND, "Endpoint does not exist")
        if not UUID_PATTERN.fullmatch(parts[0]):
            raise RequestError(HTTPStatus.BAD_REQUEST, "Invalid screenshot ID")
        return parts

    def header(self, name):
        values = self.headers.get_all(name, [])
        if len(values) > 1:
            raise RequestError(HTTPStatus.BAD_REQUEST, "Duplicate request header")
        return values[0] if values else ""

    def metadata(self):
        try:
            captured_at = timestamp(self.header("X-ScreenFrame-Captured-At"))
        except (ValueError, OverflowError) as error:
            raise RequestError(HTTPStatus.BAD_REQUEST, "Invalid capture timestamp") from error
        width = self.header("X-ScreenFrame-Width")
        height = self.header("X-ScreenFrame-Height")
        if not all(re.fullmatch(r"[0-9]{1,5}", size) for size in (width, height)):
            raise RequestError(HTTPStatus.BAD_REQUEST, "Invalid image dimensions")
        width, height = int(width), int(height)
        if not (0 < width <= MAX_SIDE and 0 < height <= MAX_SIDE and width * height <= MAX_PIXELS):
            raise RequestError(HTTPStatus.BAD_REQUEST, "Invalid image dimensions")
        device = self.header("X-ScreenFrame-Device")
        kind = self.header("X-ScreenFrame-Kind")
        if not re.fullmatch(r"[ -~]{1,64}", device) or not device.strip():
            raise RequestError(HTTPStatus.BAD_REQUEST, "Invalid device label")
        if kind not in {"full", "region"}:
            raise RequestError(HTTPStatus.BAD_REQUEST, "Invalid capture kind")
        return captured_at, width, height, device, kind

    def stage(self, length, part, media_type, metadata):
        digest = sha256()
        path = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="w+b", dir=self.server.library.directory, prefix=".upload-", delete=False
            ) as file:
                path = Path(file.name)
                remaining = length
                deadline = monotonic() + UPLOAD_TIMEOUT
                while remaining:
                    if monotonic() >= deadline:
                        raise RequestError(HTTPStatus.REQUEST_TIMEOUT, "Upload timed out")
                    chunk = self.rfile.read1(min(CHUNK, remaining))
                    if not chunk:
                        raise RequestError(HTTPStatus.BAD_REQUEST, "Incomplete image")
                    file.write(chunk)
                    digest.update(chunk)
                    remaining -= len(chunk)
                file.flush()

                size = png_size(file, length) if media_type == "image/png" else jpeg_size(file, length)
                if size is None or min(size) < 1:
                    raise RequestError(HTTPStatus.BAD_REQUEST, "Invalid image data")
                if part == "image" and size != metadata[1:3]:
                    raise RequestError(HTTPStatus.BAD_REQUEST, "Image dimensions do not match metadata")
                if part == "thumbnail" and max(size) > MAX_THUMBNAIL_SIDE:
                    raise RequestError(HTTPStatus.BAD_REQUEST, "Thumbnail dimensions exceed limit")
                os.fsync(file.fileno())
            return path, digest.hexdigest()
        except BaseException:
            if path is not None:
                path.unlink(missing_ok=True)
            raise

    def send_json(self, status, data):
        body = json.dumps(data, ensure_ascii=True, separators=(",", ":")).encode("ascii")
        self.send_headers(status, len(body), "application/json")
        self.wfile.write(body)

    def send_headers(self, status, length, media_type=None):
        self.send_response(status)
        if media_type:
            self.send_header("Content-Type", media_type)
        if status != HTTPStatus.NO_CONTENT:
            self.send_header("Content-Length", str(length))
        if status == HTTPStatus.UNAUTHORIZED:
            self.send_header("WWW-Authenticate", "Bearer")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--token-file", type=Path, required=True)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    os.umask(0o077)
    try:
        token = read_token(args.token_file)
        library = Library(args.data_dir)
        with Server(library, token, args.port) as server:
            print(f"FreezeCut library listening on 127.0.0.1:{args.port}", flush=True)
            server.serve_forever()
    except (OSError, ValueError, sqlite3.Error) as error:
        parser.exit(1, f"FreezeCut library could not start: {error}\n")
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
