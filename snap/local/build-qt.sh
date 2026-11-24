#!/bin/sh
set -eu

project=$1
build=$2
install_root=$3
inputs="$build/qt-source"
sdk="$install_root/opt/qt6"

# Snapcraft can repeat this step while retaining the part's build directory.
# Reverify the pinned inputs, then keep the original timestamps if they match.
if [ -d "$inputs/source" ]; then
    previous=$(mktemp -d "$inputs/previous.XXXXXX")
    mv "$inputs/source" "$previous/source"
    python3 "$project/scripts/release-qt.py" source "$inputs"
    if diff -qr "$previous/source" "$inputs/source" >/dev/null; then
        rm -r "$inputs/source"
        mv "$previous/source" "$inputs/source"
    fi
    rm -r "$previous"
else
    python3 "$project/scripts/release-qt.py" source "$inputs"
fi
mkdir -p "$build/qt-build"
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
