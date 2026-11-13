#!/usr/bin/env python3
"""Build the shared Qt Base SDK used in ScreenFrame release packages."""

import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("build", type=Path)
    parser.add_argument("sdk", type=Path)
    parser.add_argument("--platform", required=True, choices=("windows", "macos"))
    parser.add_argument("--arch", choices=("x86_64",), help="Build only the Intel slice on macOS")
    args = parser.parse_args()
    if args.platform == "windows" and args.arch:
        parser.error("--arch only applies to macOS")

    source, build, sdk = (path.resolve() for path in (args.source, args.build, args.sdk))
    build.mkdir(parents=True, exist_ok=False)
    options = [
        "-opensource", "-confirm-license", "-shared", "-release", "-nomake", "examples", "-nomake", "tests",
        "-no-dbus", "-no-icu", "-no-openssl", "-no-feature-zstd", "-no-feature-brotli", "-no-feature-system-libb2",
        "-qt-zlib", "-qt-pcre", "-qt-libpng", "-qt-libjpeg", "-qt-freetype", "-qt-harfbuzz",
        "-qt-doubleconversion", "-qt-libmd4c",
    ]
    if args.platform == "macos":
        configure = source / "configure"
        options.extend(["-no-feature-securetransport", "--", "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.4",
                        f"-DCMAKE_OSX_ARCHITECTURES={args.arch or 'x86_64;arm64'}"])
    else:
        configure = source / "configure.bat"
        options.extend(["-no-schannel", "--", "-DCMAKE_C_COMPILER=cl.exe", "-DCMAKE_CXX_COMPILER=cl.exe"])

    subprocess.run([str(configure), "-prefix", str(sdk), *options], cwd=build, check=True)
    jobs = str(min(8, os.cpu_count() or 1))
    subprocess.run(["cmake", "--build", str(build), "--parallel", jobs], check=True)
    subprocess.run(["cmake", "--install", str(build)], check=True)
    (sdk / "qt-build-options.txt").write_text("\n".join(options) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
