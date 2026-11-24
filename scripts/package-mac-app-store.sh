#!/bin/bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Usage:
  bash scripts/package-mac-app-store.sh stage <build-directory> <new-output-directory>
  bash scripts/package-mac-app-store.sh sandbox-test <build-directory> <new-output-directory>
  bash scripts/package-mac-app-store.sh package <build-directory> <output.pkg> \
    <app-signing-identity> <installer-signing-identity> <distribution.provisionprofile> [keychain]
EOF
    exit 2
}

fail() {
    echo "$*" >&2
    exit 1
}

[[ $# -ge 3 ]] || usage
mode=$1
case "$mode" in
    stage|sandbox-test) [[ $# == 3 ]] || usage ;;
    package) [[ $# == 6 || $# == 7 ]] && [[ $3 == *.pkg ]] || usage ;;
    *) usage ;;
esac
[[ $(uname -s) == Darwin ]] || fail "Mac App Store packaging requires macOS."

build_dir=$(cd -- "$2" && pwd -P)
grep -Eq '^FREEZECUT_MAC_APP_STORE:BOOL=(ON|TRUE|YES|1)$' "$build_dir/CMakeCache.txt" \
    || fail "Configure this build with FREEZECUT_MAC_APP_STORE=ON."
mkdir -p -- "$(dirname -- "$3")"
output_dir=$(cd -- "$(dirname -- "$3")" && pwd -P)
output="$output_dir/$(basename -- "$3")"
[[ ! -e "$output" && ! -L "$output" ]] || fail "Output already exists: $output"

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
entitlements="$script_dir/../resources/FreezeCutStore.entitlements"
work=$(mktemp -d "$output_dir/.freezecut-store.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
bundle="$work/stage/FreezeCut.app"
contents="$bundle/Contents"
plist="$contents/Info.plist"
plist_buddy=/usr/libexec/PlistBuddy

cmake --install "$build_dir" --config Release --prefix "$work/stage"
bundle_id=$("$plist_buddy" -c 'Print :CFBundleIdentifier' "$plist")
[[ $bundle_id == io.github.xopoko.Frameline ]] || fail "The bundle does not match the FreezeCut App Store record."
build_number=$("$plist_buddy" -c 'Print :CFBundleVersion' "$plist")
[[ $build_number =~ ^[1-9][0-9]*$ ]] || fail "The bundle does not have a valid App Store build number."
[[ -f "$contents/PlugIns/platforms/libqcocoa.dylib" ]] || fail "The Qt Cocoa platform plugin is missing."
[[ -f "$contents/Resources/Qt-notices/README.txt" ]] || fail "The Qt notices are missing."

executable="$contents/MacOS/FreezeCut"
architectures=$(lipo -archs "$executable")
find "$contents/MacOS" "$contents/Frameworks" "$contents/PlugIns" -type f -print0 >"$work/files"
while IFS= read -r -d '' binary; do
    if ! lipo -archs "$binary" >/dev/null 2>&1; then
        continue
    fi
    for architecture in $architectures; do
        lipo -verify_arch "$architecture" "$binary" || fail "Missing $architecture slice in $binary"
    done
    unexpected_dependencies=$(otool -L "$binary" | awk '
        /^[[:space:]]/ && $1 !~ /^(@rpath\/|@loader_path\/|@executable_path\/|\/System\/Library\/|\/usr\/lib\/)/ {
            print $1
        }')
    [[ -z "$unexpected_dependencies" ]] || fail "External dependencies in $binary: $unexpected_dependencies"
done <"$work/files"

sign_nested_code() {
    find "$contents/Frameworks" "$contents/PlugIns" -type f -name '*.dylib' -exec "$@" {} +
    find "$contents/Frameworks" -depth -type d -name '*.framework' -exec "$@" {} +
}

if [[ $mode == stage || $mode == sandbox-test ]]; then
    if [[ $mode == sandbox-test ]]; then
        # Use a separate container when testing with ad hoc signing.
        "$plist_buddy" -c "Set :CFBundleIdentifier $bundle_id.SandboxQA" "$plist"
        sign_nested_code codesign --force --sign - --timestamp=none
        codesign --force --sign - --timestamp=none --entitlements "$entitlements" "$bundle"
        codesign --verify --deep --strict "$bundle"
    fi
    mkdir -- "$output"
    mv -- "$bundle" "$output/FreezeCut.app"
    echo "Staged $output/FreezeCut.app ($architectures, build $build_number)"
    exit 0
fi

sign_code=(codesign --force --sign "$4")
make_package=(productbuild --sign "$5")
if [[ $# == 7 ]]; then
    sign_code+=(--keychain "$7")
    make_package+=(--keychain "$7")
fi

profile="$contents/embedded.provisionprofile"
cp -- "$6" "$profile"
profile_plist="$work/profile.plist"
security cms -D -i "$profile" -o "$profile_plist"
if "$plist_buddy" -c 'Print :ProvisionedDevices' "$profile_plist" >/dev/null 2>&1 \
    || [[ $("$plist_buddy" -c 'Print :ProvisionsAllDevices' "$profile_plist" 2>/dev/null || true) == true ]] \
    || [[ $("$plist_buddy" -c 'Print :Entitlements:com.apple.security.get-task-allow' "$profile_plist" 2>/dev/null || true) == true ]]; then
    fail "Use a Mac App Store distribution provisioning profile."
fi

application_id=$("$plist_buddy" -c 'Print :Entitlements:com.apple.application-identifier' "$profile_plist")
team_id=$("$plist_buddy" -c 'Print :TeamIdentifier:0' "$profile_plist")
[[ $application_id == *."$bundle_id" ]] || fail "The provisioning profile belongs to another app."
signed_entitlements="$work/FreezeCut.entitlements"
cp -- "$entitlements" "$signed_entitlements"
"$plist_buddy" -c "Add :com.apple.application-identifier string $application_id" "$signed_entitlements"
"$plist_buddy" -c "Add :com.apple.developer.team-identifier string $team_id" "$signed_entitlements"

sign_nested_code "${sign_code[@]}"
"${sign_code[@]}" --entitlements "$signed_entitlements" "$bundle"
codesign --verify --deep --strict "$bundle"
if ! codesign --display --verbose=4 "--extract-certificates=$work/identity-" "$bundle" 2>"$work/signature"; then
    cat "$work/signature" >&2
    fail "Could not inspect the app distribution signature."
fi
grep -Eq '^Authority=(Apple Distribution|3rd Party Mac Developer Application):' "$work/signature" \
    && grep -Fxq "TeamIdentifier=$team_id" "$work/signature" \
    || fail "Use the app distribution signing identity for the profile's Apple team."
plutil -extract DeveloperCertificates.0 raw -o - "$profile_plist" | base64 -D >"$work/profile-certificate.der"
cmp -s "$work/identity-0" "$work/profile-certificate.der" \
    || fail "The app signing certificate does not match the provisioning profile."

package="$work/FreezeCut.pkg"
"${make_package[@]}" --component "$bundle" /Applications "$package"
pkgutil --check-signature "$package" >"$work/installer-signature"
grep -Fq '3rd Party Mac Developer Installer:' "$work/installer-signature" \
    || fail "Use a Mac Installer Distribution signing identity."
ln -- "$package" "$output"
echo "Created $output ($architectures, build $build_number)"
