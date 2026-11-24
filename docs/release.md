# Release guide

Build and test the release candidate from a clean checkout. The repository's **Build and test** workflow checks the desktop application on Windows, macOS, and Ubuntu. In GitHub Actions, select **Prepare release artifacts**, choose the candidate branch or tag, and run the workflow. Download the packages and corresponding Qt source from the workflow run.

| Package | Target | Notes |
| --- | --- | --- |
| ZIP | Windows 10 (1809 or later) and Windows 11, x64 | Extract before running `bin/FreezeCutGUI.exe`. The preview is unsigned. |
| DMG | macOS 14.4+, Apple Silicon and Intel | The preview is ad hoc signed. Validate a quarantined download before public distribution. |
| DEB | Ubuntu 24.04, amd64 | Uses system Qt packages. Capture works in X11 or through a compatible Wayland portal. |

Use a fresh profile and sample screenshots for these checks:

1. Verify full-screen capture, region selection, save, copy, and keyboard navigation before configuring a library.
2. Capture a sample screenshot with **Sync across devices** set to **Local only** and open **Library** while the editor is still open. Check that the capture appears. Quit and reopen FreezeCut, then check that the capture is still available.
3. Under **Settings → Sync across devices → Host here**, select **Create shared library**. Enable **Access from other devices**, apply sharing, copy the code and use **Connect** on another computer signed in to the same Tailscale account. Verify browsing and automatic upload in **Shared**. Disconnect Tailscale, check that local captures remain available and uploads wait in the queue, then reconnect and confirm they arrive. Turn sharing off and verify local history again.
4. Enable **Start FreezeCut when I sign in**, sign out and back in, and verify FreezeCut starts in the tray and serves the library. Launch the app again and check that the running instance opens its window. Disable the setting and verify it stays off at the next login.
5. Connect to a disposable standalone library over SSH and verify browsing and automatic upload. Confirm that upgrading preserves settings and queued captures, and that queued captures stay with their original library when you switch destinations.

## Windows and macOS dependencies

The release workflow builds shared Qt Base 6.12.0 from the official source with Qt's patches for CVE-2026-105924, CVE-2026-105943, and CVE-2026-116337. It verifies the source and patches by SHA-256 before building. [Qt Base security patches](https://download.qt.io/official_releases/qt/6.12/)

Publish the corresponding Qt source bundle alongside the Windows and macOS packages. It contains the upstream archive, the applied patches, their checksums, and the build recipe. Keep the included license texts, attribution, and library replacement instructions. [Qt redistribution obligations](https://www.qt.io/development/open-source-lgpl-obligations)

For a public macOS download, use a Developer ID signature and Apple's notarization service, then verify the downloaded package on a clean Mac. The artifact workflow stages ad hoc builds for testing. [Apple notarization guide](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)

## Mac App Store

Use an Xcode-equipped Mac with CMake, Ninja and Python 3.12 or later. Prepare the pinned Qt inputs and build the shared SDK with App Store support. The default macOS SDK build includes Apple Silicon and Intel.

```sh
python3 scripts/release-qt.py source release-qt
python3 scripts/build-release-qt.py release-qt/source release-qt/build release-qt/sdk --platform macos --app-store
python3 scripts/release-qt.py notices release-qt/source release-qt/sdk release-qt/notices
cmake -S . -B build-store -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64' \
  -DCMAKE_PREFIX_PATH="$PWD/release-qt/sdk" \
  -DFREEZECUT_QT_NOTICES_DIR="$PWD/release-qt/notices" \
  -DFREEZECUT_MAC_APP_STORE=ON -DFREEZECUT_MAC_BUILD_NUMBER=1
cmake --build build-store --parallel
ctest --test-dir build-store --output-on-failure
```

Use `bash scripts/package-mac-app-store.sh sandbox-test build-store /path/to/new-test-directory` to produce a locally signed app with a separate test container. To build with a modified Qt, edit the prepared Qt source, choose new `build` and `sdk` paths, then repeat the shared SDK and app builds. Prepare notices again from the modified source. The local test app runs independently of App Store receipts or distribution credentials. Grant it Screen Recording permission before testing capture.

Sign a distribution package with the existing Apple Distribution and Mac Installer Distribution identities and a Mac App Store provisioning profile for `io.github.xopoko.Frameline`. Run `scripts/package-mac-app-store.sh` without arguments to see the supported commands. Test native region capture, PNG saving, clipboard copy, Library persistence and login behavior before sending the candidate to review.

The Store build keeps its Library in its App Sandbox container. To carry over a history from a direct download, quit both apps and copy captures from `~/.frameline/history` to the Store build's `history` folder in its container. The [privacy policy](../PRIVACY.md) lists the container path.

The Mac App Store build supports local and Tailscale library connections. Use the direct download for SSH libraries, which need access to your existing SSH configuration and keys.

Publish the exact application source and the matching Qt source archive under `v<version>-mac-app-store.<build>` on GitHub. The menu bar's **Open Source Licenses** links to that release and opens the bundled license notices. Use `python3 scripts/release-qt.py archive release-qt dist` to generate the corresponding Qt archive.

## Ubuntu dependencies

The DEB uses the Qt libraries from Ubuntu 24.04. Install Ubuntu's security updates to receive fixes for these libraries. Canonical tracks the status of [CVE-2026-105924](https://ubuntu.com/security/CVE-2026-105924) and [CVE-2026-116337](https://ubuntu.com/security/CVE-2026-116337) for its `qt6-base` package.

## Before making a release public

Review Git history, package contents, and screenshots for private information. Check the downloaded ZIP, DMG, and DEB on the target architectures and publish only the verified files and their checksums. Enable private vulnerability reporting before directing users to it. See [Security](../SECURITY.md).
