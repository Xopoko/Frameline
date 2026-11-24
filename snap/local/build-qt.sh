#!/bin/sh
set -eu

project=$1
build=$2
install_root=$3
inputs="$build/qt-source"
sdk="$install_root/opt/qt6"

python3 "$project/scripts/release-qt.py" source "$inputs"
mkdir "$build/qt-build"
cd "$build/qt-build"
set -- -opensource -confirm-license -shared -release -nomake examples -nomake tests \
    -xcb -feature-wayland -dbus -opengl desktop -no-icu -no-openssl \
    -no-feature-zstd -no-feature-brotli -no-feature-system-libb2 \
    -qt-zlib -qt-pcre -qt-libpng -qt-libjpeg -qt-freetype -qt-harfbuzz \
    -qt-doubleconversion -qt-libmd4c
"$inputs/source/configure" -prefix "$sdk" "$@"
cmake --build . --parallel 12
cmake --install .
printf '%s\n' "$@" > "$sdk/qt-build-options.txt"
python3 "$project/scripts/release-qt.py" notices "$inputs/source" "$sdk" \
    "$install_root/usr/share/doc/freezecut/Qt-notices"
