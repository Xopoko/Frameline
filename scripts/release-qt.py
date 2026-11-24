#!/usr/bin/env python3
"""Prepare the pinned Qt Base source, patches, and redistribution files."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen


QT_VERSION = "6.12.0"
QT_SOURCE = f"qtbase-everywhere-src-{QT_VERSION}"
QT_SITE = "https://download.qt.io"
QT_SERIES = "6.12"
SOURCE_NAME = f"{QT_SOURCE}.tar.xz"
SOURCE_PATH = f"archive/qt/{QT_SERIES}/{QT_VERSION}/submodules/{SOURCE_NAME}"
SOURCE_SHA256 = "a951bd163c7b80fc6b8c88d7668fb56abf91c152373e13c10666763238131307"
PATCHES = (
    (
        "CVE-2026-105924-qtbase-6.12.diff",
        "c27d27b86aaf2cdab0c18d513fbb94a432e98782eddfc5f856387e4be68be50b",
    ),
    (
        "CVE-2026-105943-qtbase-6.12.diff",
        "dd22629ce948cf0a900421323cca331907180242a9b51c3fd75b25c75f068099",
    ),
    # The test changes in 0001 depend on 0002, despite the filenames.
    (
        "CVE-2026-116337-qtbase-6.12-0002.diff",
        "83abc89d507dbd9a94dc5718b0125492c1c37b0609a8f404a1f164925940faec",
    ),
    (
        "CVE-2026-116337-qtbase-6.12-0001.diff",
        "74c5028985dcc0455255909de4dc80f50cf5472dbf465ce46ebc791a7b080f12",
    ),
)
SCRIPTS = Path(__file__).resolve().parent


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


def source_inputs(directory):
    return [(directory / SOURCE_NAME, SOURCE_SHA256)] + [
        (directory / "patches" / name, digest) for name, digest in PATCHES
    ]


def prepare_source(directory):
    directory = directory.resolve()
    output = directory / "source"
    if output.exists() or output.is_symlink():
        raise FileExistsError(f"Source directory already exists: {output}")

    (archive, digest), *patches = source_inputs(directory)
    fetch(SOURCE_PATH, digest, archive)
    for patch, digest in patches:
        fetch(f"official_releases/qt/{QT_SERIES}/{patch.name}", digest, patch)

    with tempfile.TemporaryDirectory(dir=directory) as work:
        subprocess.run(["cmake", "-E", "tar", "xf", str(archive)], cwd=work, check=True)
        stage = Path(work) / QT_SOURCE
        # Keep Git from discovering an enclosing FreezeCut checkout when applying Qt paths.
        environment = {**os.environ, "GIT_CEILING_DIRECTORIES": Path(work).as_posix()}
        for patch, _ in patches:
            subprocess.run(["git", "-c", "core.autocrlf=false", "apply", "--", str(patch)],
                           cwd=stage, env=environment, check=True)
        stage.rename(output)


def archive_source(directory, output):
    inputs = source_inputs(directory)
    for path, digest in inputs:
        verify(path, digest)

    instructions = (
        f"# FreezeCut's Qt Base {QT_VERSION} source\n\n"
        "The upstream archive and all Qt patches used to build the bundled shared libraries are included.\n"
        "The source preparation script verifies the included inputs and applies the patches in dependency order.\n\n"
        "From this directory, run `python scripts/release-qt.py source .`.\n"
        "On Windows x64, open an x64 Visual Studio developer shell and run\n"
        "`python scripts/build-release-qt.py source build sdk --platform windows`.\n"
        "On macOS with Xcode, run `python3 scripts/build-release-qt.py source build sdk --platform macos`.\n"
        "Both builds require Python 3.12, Git, CMake, and Ninja.\n"
    )
    manifest = "".join(f"{digest}  {path.relative_to(directory).as_posix()}\n" for path, digest in inputs)
    output.mkdir(parents=True, exist_ok=True)
    filename = f"qtbase-{QT_VERSION}-freezecut-source.tar.xz"
    with tempfile.TemporaryDirectory(dir=output) as work:
        bundle = Path(work) / filename
        with tarfile.open(bundle, "w:xz", preset=0) as archive:
            for path, _ in inputs:
                archive.add(path, path.relative_to(directory).as_posix(), filter=portable_metadata)
            for name in ("release-qt.py", "build-release-qt.py", "qt-notice.txt"):
                archive.add(SCRIPTS / name, f"scripts/{name}", filter=portable_metadata)
            for name, content in (("README.md", instructions), ("SHA256SUMS", manifest)):
                data = content.encode("utf-8")
                entry = tarfile.TarInfo(name)
                entry.size = len(data)
                archive.addfile(entry, io.BytesIO(data))
        bundle.replace(output / filename)


def portable_metadata(entry):
    entry.uid = entry.gid = entry.mtime = 0
    entry.uname = entry.gname = ""
    entry.mode = 0o644
    return entry


def collect_notices(source, sdk, output):
    if not (source / "LICENSES" / "LGPL-3.0-only.txt").is_file():
        raise ValueError(f"Missing Qt Base license texts under {source}")
    output.mkdir(parents=True)
    entries = []
    source_notices = output / "upstream"
    for upstream in source.rglob("*"):
        path = upstream.relative_to(source)
        is_license = path.parts[0] == "LICENSES" or (
            "3rdparty" in path.parts
            and path.name.upper().startswith(("LICENSE", "COPYING", "COPYRIGHT", "NOTICE", "FTL.TXT"))
        )
        is_attribution = path.name == "qt_attribution.json"
        if not (is_license or is_attribution) or not upstream.is_file():
            continue
        content = upstream.read_bytes()
        dest = source_notices / path
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(content)
        if is_attribution:
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
    sbom = sdk / "sbom" / f"qtbase-{QT_VERSION}.spdx.json"
    if sbom.is_file():
        shutil.copyfile(sbom, output / "qtbase.spdx.json")
    shutil.copyfile(sdk / "qt-build-options.txt", output / "qt-build-options.txt")
    template = SCRIPTS / "qt-notice.txt"
    text = template.read_text(encoding="utf-8").replace("@QT_VERSION@", QT_VERSION)
    (output / "README.txt").write_text(text, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    source = subparsers.add_parser("source")
    source.add_argument("output", type=Path, help="Workspace for the pinned inputs and patched source")
    archive = subparsers.add_parser("archive")
    archive.add_argument("workspace", type=Path, help="Workspace with the verified source inputs")
    archive.add_argument("output", type=Path, help="Directory for the corresponding source bundle")
    notices = subparsers.add_parser("notices")
    notices.add_argument("source", type=Path, help="Patched Qt Base source directory")
    notices.add_argument("sdk", type=Path, help="Built Qt SDK directory")
    notices.add_argument("output", type=Path, help="New notices directory")
    args = parser.parse_args()
    if args.command == "source":
        prepare_source(args.output)
    elif args.command == "archive":
        archive_source(args.workspace, args.output)
    else:
        collect_notices(args.source, args.sdk, args.output)


if __name__ == "__main__":
    main()
