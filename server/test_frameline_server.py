from concurrent.futures import ThreadPoolExecutor
from hashlib import sha256
from http.client import HTTPConnection
import json
import os
from pathlib import Path
import struct
from tempfile import TemporaryDirectory
from threading import Thread
from time import time
import unittest
from urllib.parse import urlencode
import zlib

from frameline_server import Library, MAX_IMAGE, Server, read_token


ROOT = "/v1/screenshots"
TOKEN = b"test-token-private-12345678901234567890"


def image_id(index):
    return f"00000000-0000-4000-8000-{index:012x}"


def png(color=b"\xff\0\0"):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(b"\0" + color))
        + chunk(b"IEND", b"")
    )


def jpeg():
    # 1×1 gray JPEG with one quantization table and two single-code Huffman tables.
    quantization = b"\xff\xdb\0C\0" + bytes([1] * 64)
    frame = b"\xff\xc0\0\x0b\x08\0\x01\0\x01\x01\x01\x11\0"
    tables = b"\xff\xc4\0\x26"
    for table in (0x00, 0x10):
        tables += bytes([table, 1] + [0] * 16)
    scan = b"\xff\xda\0\x08\x01\x01\0\0\x3f\0\x3f"
    return b"\xff\xd8" + quantization + frame + tables + scan + b"\xff\xd9"


class LibraryApiTest(unittest.TestCase):
    def setUp(self):
        self.temp = TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.library = Library(Path(self.temp.name) / "library")
        self.server = Server(self.library, TOKEN, 0)
        self.addCleanup(self.server.server_close)
        self.thread = Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.01})
        self.thread.start()
        self.addCleanup(self.thread.join)
        self.addCleanup(self.server.shutdown)

    def request(self, method, path, body=None, headers=None, auth=True):
        headers = dict(headers or {})
        if auth:
            headers["Authorization"] = f"Bearer {TOKEN.decode('ascii')}"
        connection = HTTPConnection(*self.server.server_address, timeout=4)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def put_image(self, index=1, body=None, **headers):
        attributes = {
            "Content-Type": "image/png",
            "X-ScreenFrame-Captured-At": "2026-11-13T19:17:00.120+01:00",
            "X-ScreenFrame-Width": "1",
            "X-ScreenFrame-Height": "1",
            "X-ScreenFrame-Device": "Windows",
            "X-ScreenFrame-Kind": "region",
        }
        attributes.update(headers)
        return self.request("PUT", f"{ROOT}/{image_id(index)}/image", body or png(), attributes)[0]

    def test_private_list_and_files(self):
        self.assertEqual(self.server.server_address[0], "127.0.0.1")
        status, _, body = self.request("GET", "/healthz", auth=False)
        self.assertEqual((status, json.loads(body)), (200, {"status": "ok"}))
        for path in (ROOT, f"{ROOT}/{image_id(1)}/image", f"{ROOT}/{image_id(1)}/thumbnail"):
            status, headers, _ = self.request("GET", path, auth=False)
            self.assertEqual((status, headers["WWW-Authenticate"]), (401, "Bearer"))
            self.assertEqual(self.request("GET", path, headers={"Authorization": "Bearer wrong"}, auth=False)[0], 401)

    def test_upload_download_and_immutable_retry(self):
        path = f"{ROOT}/{image_id(1)}"
        thumb_headers = {"Content-Type": "image/png"}
        self.assertEqual(self.request("PUT", path + "/thumbnail", png(), thumb_headers)[0], 404)
        self.assertEqual(self.put_image(), 201)
        self.assertEqual(self.put_image(**{"X-ScreenFrame-Captured-At": "2026-11-13T18:17:00.120Z"}), 204)
        self.assertEqual(self.put_image(body=png(b"\0\xff\0")), 409)
        self.assertEqual(self.put_image(**{"X-ScreenFrame-Device": "Mac"}), 409)
        self.assertEqual(self.request("GET", path + "/thumbnail")[0], 404)
        for status in (201, 204):
            self.assertEqual(self.request("PUT", path + "/thumbnail", png(), thumb_headers)[0], status)
        self.assertEqual(self.request("PUT", path + "/thumbnail", png(b"\0\xff\0"), thumb_headers)[0], 409)
        for part in ("image", "thumbnail"):
            status, headers, body = self.request("GET", path + "/" + part)
            self.assertEqual((status, headers["Content-Type"], headers["Cache-Control"], body),
                             (200, "image/png", "no-store", png()))
        status, _, body = self.request("GET", ROOT)
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {"items": [{
            "id": image_id(1), "capturedAt": "2026-11-13T18:17:00.120Z",
            "width": 1, "height": 1, "device": "Windows", "kind": "region", "hasThumbnail": True,
        }], "nextCursor": None})
        if os.name == "posix":
            self.assertEqual(self.library.path_for(image_id(1), "image").stat().st_mode & 0o077, 0)

    def test_bounds_and_validation(self):
        path = f"{ROOT}/{image_id(1)}/image"
        self.assertEqual(self.put_image(**{"Content-Length": str(MAX_IMAGE + 1)}), 413)
        self.assertEqual(self.put_image(**{"Content-Type": "text/html"}), 415)
        self.assertEqual(self.put_image(**{"X-ScreenFrame-Width": "2"}), 400)
        self.assertEqual(self.put_image(**{"X-ScreenFrame-Captured-At": "yesterday"}), 400)
        self.assertEqual(self.put_image(**{"X-ScreenFrame-Kind": "screen"}), 400)
        self.assertEqual(self.put_image(body=b"not PNG"), 400)
        self.assertEqual(self.put_image(body=png()[:33] + png()[-12:]), 400)
        self.assertEqual(self.request("PUT", path, png(), {"Content-Type": "image/png"}, auth=False)[0], 401)
        self.assertEqual(self.request("GET", f"{ROOT}/%2e%2e/image")[0], 400)
        self.assertEqual(self.request("GET", ROOT + "?limit=101")[0], 400)
        self.assertEqual(self.request("GET", ROOT + "?cursor=bad")[0], 400)
        self.assertEqual(self.request("GET", path)[0], 404)
        self.assertFalse(list(self.library.directory.glob(".upload-*")))

    def test_jpeg_thumbnail(self):
        path = f"{ROOT}/{image_id(1)}/thumbnail"
        self.assertEqual(self.put_image(), 201)
        self.assertEqual(self.request("PUT", path, jpeg(), {"Content-Type": "image/jpeg"})[0], 201)
        status, headers, body = self.request("GET", path)
        self.assertEqual((status, headers["Content-Type"], body), (200, "image/jpeg", jpeg()))

    def test_capture_timestamp_normalizes_fractional_precision(self):
        values = (
            ("2026-11-13T18:17:00Z", "2026-11-13T18:17:00.000Z"),
            ("2026-11-13T18:17:00.1Z", "2026-11-13T18:17:00.100Z"),
            ("2026-11-13T19:17:00.12345+01:00", "2026-11-13T18:17:00.123Z"),
            ("2026-11-13T18:17:00.123456789Z", "2026-11-13T18:17:00.123Z"),
        )
        for index, (submitted, expected) in enumerate(values, 1):
            self.assertEqual(self.put_image(index, **{"X-ScreenFrame-Captured-At": submitted}), 201)
            self.assertEqual(self.put_image(index, **{"X-ScreenFrame-Captured-At": expected}), 204)
        items = json.loads(self.request("GET", ROOT)[2])["items"]
        self.assertEqual({item["id"]: item["capturedAt"] for item in items},
                         {image_id(index): expected for index, (_, expected) in enumerate(values, 1)})

    def test_pagination_with_equal_timestamps_and_insertion(self):
        for index in (1, 2, 3):
            self.assertEqual(self.put_image(index), 201)
        first = json.loads(self.request("GET", ROOT + "?limit=2")[2])
        self.assertEqual([item["id"] for item in first["items"]], [image_id(3), image_id(2)])
        self.assertEqual(self.put_image(4), 201)
        query = urlencode({"limit": 2, "cursor": first["nextCursor"]})
        second = json.loads(self.request("GET", f"{ROOT}?{query}")[2])
        self.assertEqual([item["id"] for item in second["items"]], [image_id(1)])
        self.assertIsNone(second["nextCursor"])

    def test_concurrent_uploads_and_store_reopen(self):
        with ThreadPoolExecutor(max_workers=4) as pool:
            self.assertEqual(sorted(pool.map(self.put_image, [1] * 4)), [201, 204, 204, 204])
        abandoned = self.library.directory / ".upload-old"
        abandoned.touch()
        yesterday = time() - 86_401
        os.utime(abandoned, (yesterday, yesterday))
        reopened = Library(self.library.directory)
        self.assertFalse(abandoned.exists())
        file, media = reopened.open(image_id(1), "image")
        with file:
            self.assertEqual((media, sha256(file.read()).digest()), ("image/png", sha256(png()).digest()))

    def test_token_file(self):
        path = Path(self.temp.name) / "token"
        path.write_bytes(TOKEN + b"\n")
        path.chmod(0o600)
        self.assertEqual(read_token(path), TOKEN)
        path.write_bytes(b"short-token\n")
        with self.assertRaises(ValueError):
            read_token(path)
        if os.name == "posix":
            path.write_bytes(TOKEN)
            path.chmod(0o644)
            with self.assertRaises(ValueError):
                read_token(path)


if __name__ == "__main__":
    unittest.main()
