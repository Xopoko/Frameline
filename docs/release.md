# Release guide

Build and test the release candidate from a clean checkout. The repository's **Build and test** workflow checks the desktop application on Windows, macOS, and Ubuntu. In GitHub Actions, select **Prepare release artifacts**, choose the candidate branch or tag, and run the workflow. Download the packages and corresponding Qt source from the workflow run.

| Package | Target | Notes |
| --- | --- | --- |
| ZIP | Windows 10 (1809 or later) and Windows 11, x64 | Extract before running `bin/ScreenFrameGUI.exe`. The preview is unsigned. |
| DMG | macOS 14.4+, Apple Silicon and Intel | The preview is ad hoc signed. Validate a quarantined download before public distribution. |
| DEB | Ubuntu 24.04, amd64 | Uses system Qt packages. Capture works in X11 or through a compatible Wayland portal. |

Test first launch with an empty profile and no library server. Verify capture, region selection, save, copy, and keyboard navigation. Then connect to a disposable library over SSH and verify both browsing and automatic upload. Confirm that settings and queued files survive an upgrade and that switching libraries never transfers queued files to the new server.

## Windows and macOS dependencies

The release workflow builds shared Qt Base 6.12.0 from the official source with Qt's patches for CVE-2026-105924, CVE-2026-105943, and CVE-2026-116337. It verifies the source and patches by SHA-256 before building. [Qt Base security patches](https://download.qt.io/official_releases/qt/6.12/)

Publish the corresponding Qt source bundle alongside the Windows and macOS packages. It contains the upstream archive, the applied patches, their checksums, and the build recipe. Keep the included license texts, attribution, and library replacement instructions. [Qt redistribution obligations](https://www.qt.io/development/open-source-lgpl-obligations)

For a public macOS download, use a Developer ID signature and Apple's notarization service, then verify the downloaded package on a clean Mac. The artifact workflow stages ad hoc builds for testing. [Apple notarization guide](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)

## Ubuntu dependencies

The DEB uses the Qt libraries from Ubuntu 24.04. Install Ubuntu's security updates to receive fixes for these libraries. Canonical tracks the status of [CVE-2026-105924](https://ubuntu.com/security/CVE-2026-105924) and [CVE-2026-116337](https://ubuntu.com/security/CVE-2026-116337) for its `qt6-base` package.

## Before making a release public

Review Git history, package contents, and screenshots for private information. Check the downloaded ZIP, DMG, and DEB on the target architectures and publish only the verified files and their checksums. Enable private vulnerability reporting before directing users to it. See [Security](../SECURITY.md).
