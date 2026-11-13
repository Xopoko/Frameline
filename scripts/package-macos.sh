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
# PNG is built in. Keep JPEG support for library thumbnails.
rm -f -- "$plugins"/imageformats/libq{gif,heif,icns,ico,svg,tga,tiff,wbmp,webp}.dylib \
    "$plugins/iconengines/libqsvgicon.dylib"
rm -rf -- "$plugins/generic" "$plugins/networkinformation" "$plugins/tls"

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
        if [[ "$architectures" != *' '* && "$slices" != "$architectures" ]]; then
            lipo "$binary" -thin "$architectures" -output "$binary.thin"
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
if [[ " $architectures " == *" $(uname -m) "* ]]; then
    "$executable" --version
else
    echo "Built for $architectures. Runtime smoke test requires that architecture."
fi

ln -s /Applications "$stage/Applications"
image="$work/ScreenFrame.dmg"
for attempt in 1 2 3; do
    if hdiutil create -fs HFS+ -format UDZO -volname ScreenFrame -srcfolder "$stage" "$image" >"$work/create.log" 2>&1; then
        cat "$work/create.log"
        break
    fi
    cat "$work/create.log" >&2
    if [[ "$attempt" == 3 ]] || ! grep -q 'Resource busy' "$work/create.log"; then
        exit 1
    fi
    rm -f -- "$image"
    sleep "$((attempt * 3))"
done
hdiutil verify "$image"
ln -- "$image" "$output"
echo "Created $output"
