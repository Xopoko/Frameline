#!/usr/bin/env python3
"""Fetch the pinned release SDK and collect its redistribution notices."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen


QT_VERSION = "6.12.0"
QT_SOURCE = f"qtbase-everywhere-src-{QT_VERSION}"
QT_SITE = "https://download.qt.io"
SOURCE_PATH = f"archive/qt/6.12/{QT_VERSION}/submodules/{QT_SOURCE}.tar.xz"
SOURCE_SHA256 = "a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307"
SDK_ARCHIVES = {
    "windows": (
        "windows_x86/desktop/qt6_6120/qt6_6120_msvc2022_64/qt.qt6.6120.win64_msvc2022_64/"
        "6.12.0-0-202609280346qtbase-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z",
        "219d1b86def3be418e31d704572bc4acc91b2ec373c65595c9fb12b42bf098dc",
    ),
    "macos": (
        "mac_x64/desktop/qt6_6120/qt6_6120/qt.qt6.6120.clang_64/"
        "6.12.0-0-202609280346qtbase-MacOS-MacOS_26-Clang-MacOS-MacOS_26-X86_64-ARM64.7z",
        "1e0df66ed17ae97997971ff481983c5c0b7497a52ef52f699725701401fd80ee",
    ),
}


def verify(path, expected_hash):
    with path.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != expected_hash:
        raise ValueError(f"SHA-256 mismatch for {path.name}")


def fetch(remote_path, expected_hash, output):
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        verify(output, expected_hash)
        return
    with tempfile.TemporaryDirectory(dir=output.parent) as work:
        download = Path(work) / output.name
        with urlopen(f"{QT_SITE}/{remote_path}", timeout=90) as response, download.open("wb") as stream:
            shutil.copyfileobj(response, stream)
        verify(download, expected_hash)
        download.replace(output)


def install_sdk(platform, output):
    remote_path, digest = SDK_ARCHIVES[platform]
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as work:
        archive = Path(work) / "qtbase.7z"
        fetch(f"online/qtsdkrepository/{remote_path}", digest, archive)
        stage = Path(work) / "sdk"
        stage.mkdir()
        subprocess.run(["cmake", "-E", "tar", "xf", str(archive.resolve())], cwd=stage, check=True)
        if platform == "windows":
            # The installer normally writes this relocation setting after extraction.
            (stage / "bin/qt.conf").write_text("[Paths]\nPrefix=..\n", encoding="ascii")
        stage.rename(output)


def collect_notices(source, sdk, output):
    verify(source, SOURCE_SHA256)
    output.mkdir(parents=True)
    entries = []
    source_notices = output / "upstream"
    with tarfile.open(source, "r:xz") as archive:
        for member in archive:
            path = PurePosixPath(member.name).relative_to(QT_SOURCE)
            if not member.isfile():
                continue
            is_license = path.parts[0] == "LICENSES" or (
                "3rdparty" in path.parts
                and path.name.upper().startswith(("LICENSE", "COPYING", "COPYRIGHT", "NOTICE", "FTL.TXT"))
            )
            if path.name != "qt_attribution.json" and not is_license:
                continue
            content = archive.extractfile(member).read()
            dest = source_notices.joinpath(*path.parts)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(content)
            if path.name == "qt_attribution.json":
                # Upstream attribution files can contain literal control characters in prose.
                records = json.loads(content, strict=False)
                entries.extend(records if isinstance(records, list) else [records])

    with (output / "THIRD-PARTY.txt").open("w", encoding="utf-8") as stream:
        stream.write(f"Qt Base {QT_VERSION} upstream attributions\n\n")
        for record in sorted(entries, key=lambda entry: entry["Name"].casefold()):
            stream.write(f"{record['Name']}\n")
            for field in ("License", "Copyright"):
                value = record.get(field, [])
                stream.write("\n".join(value if isinstance(value, list) else [value]) + "\n")
            stream.write("\n")
    shutil.copyfile(sdk / "sbom" / f"qtbase-{QT_VERSION}.spdx.json", output / "qtbase.spdx.json")
    shutil.copyfile(sdk / "config_qtbase.opt", output / "qt-build-options.txt")
    template = Path(__file__).resolve().with_name("qt-notice.txt")
    text = template.read_text(encoding="utf-8").replace("@QT_VERSION@", QT_VERSION)
    (output / "README.txt").write_text(text, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    source = subparsers.add_parser("source")
    source.add_argument("output", type=Path, help="Directory for the corresponding source archive")
    sdk = subparsers.add_parser("sdk")
    sdk.add_argument("platform", choices=SDK_ARCHIVES)
    sdk.add_argument("output", type=Path, help="New Qt SDK directory")
    notices = subparsers.add_parser("notices")
    notices.add_argument("source", type=Path, help="Qt Base source archive")
    notices.add_argument("sdk", type=Path, help="Extracted Qt SDK directory")
    notices.add_argument("output", type=Path, help="New notices directory")
    args = parser.parse_args()
    if args.command == "source":
        output = args.output / f"{QT_SOURCE}.tar.xz"
        fetch(SOURCE_PATH, SOURCE_SHA256, output)
        print(output)
    elif args.command == "sdk":
        install_sdk(args.platform, args.output)
    else:
        collect_notices(args.source, args.sdk, args.output)


if __name__ == "__main__":
    main()
