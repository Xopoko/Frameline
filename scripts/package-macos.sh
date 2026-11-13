#!/bin/bash
set -euo pipefail

if [[ $# != 2 || "$2" != *.dmg ]]; then
    echo "Usage: bash scripts/package-macos.sh <build-directory> <output.dmg>" >&2
    exit 2
fi

build_dir=$(cd -- "$1" && pwd -P)
mkdir -p -- "$(dirname -- "$2")"
output_dir=$(cd -- "$(dirname -- "$2")" && pwd -P)
output="$output_dir/$(basename -- "$2")"
if [[ -e "$output" || -L "$output" ]]; then
    echo "Output already exists: $output" >&2
    exit 1
fi

work=$(mktemp -d "$output_dir/.screenframe-package.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
stage="$work/stage"
cmake --install "$build_dir" --config Release --prefix "$stage"
bundle="$stage/ScreenFrame.app"
plugins="$bundle/Contents/PlugIns"
frameworks="$bundle/Contents/Frameworks"
executable="$bundle/Contents/MacOS/ScreenFrame"

test -f "$plugins/platforms/libqcocoa.dylib"
# The editor uses the built-in PNG handler and the Cocoa platform plugin.
rm -f -- "$plugins"/imageformats/libq{gif,ico,jpeg,svg}.dylib "$plugins/iconengines/libqsvgicon.dylib"

architectures=$(lipo -archs "$executable")
find "$bundle/Contents" -type f -perm -u=x -print0 >"$work/executables"
while IFS= read -r -d '' binary; do
    if slices=$(lipo -archs "$binary" 2>/dev/null); then
        for architecture in $architectures; do
            if [[ " $slices " != *" $architecture "* ]]; then
                echo "Missing $architecture slice in $binary" >&2
                exit 1
            fi
        done
        if [[ "$architectures" == arm64 && "$slices" != arm64 ]]; then
            lipo "$binary" -thin arm64 -output "$binary.thin"
            chmod "$(stat -f '%Lp' "$binary")" "$binary.thin"
            mv -f -- "$binary.thin" "$binary"
        fi
    fi
done <"$work/executables"

find "$plugins" "$frameworks" -type f -name '*.dylib' \
    -exec codesign --force --sign - --timestamp=none {} +
find "$frameworks" -maxdepth 1 -type d -name '*.framework' \
    -exec codesign --force --sign - --timestamp=none {} +
codesign --force --sign - --timestamp=none "$bundle"
codesign --verify --deep --strict "$bundle"
"$executable" --version

ln -s /Applications "$stage/Applications"
image="$work/ScreenFrame.dmg"
hdiutil create -fs HFS+ -format UDZO -volname ScreenFrame -srcfolder "$stage" "$image"
hdiutil verify "$image"
ln -- "$image" "$output"
echo "Created $output"
