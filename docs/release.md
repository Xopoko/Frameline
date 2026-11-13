# Release guide

Build and test the release candidate from a clean checkout. The repository's **Build and test** workflow checks the desktop application on Windows, macOS, and Ubuntu. In GitHub Actions, select **Prepare release artifacts**, choose the candidate branch or tag, and run the workflow. Download the packages and corresponding Qt source from the workflow run.

| Package | Target | Notes |
| --- | --- | --- |
| ZIP | Windows 10 (1809 or later) and Windows 11, x64 | Extract before running `bin/ScreenFrameGUI.exe`. The preview is unsigned. |
| DMG | macOS 14.4+, Apple Silicon and Intel | The preview is ad hoc signed. Validate a quarantined download before public distribution. |
| DEB | Ubuntu 24.04, amd64 | Uses system Qt packages. Capture works in X11 or through a compatible Wayland portal. |

Test first launch with an empty profile and no library server. Verify capture, region selection, save, copy, and keyboard navigation. Then connect to a disposable library over SSH and verify both browsing and automatic upload. Confirm that old settings and queued files continue to work after an upgrade and that switching libraries never transfers queued files to the new server.

## Windows and macOS dependencies

The release workflow uses a pinned official shared Qt Base SDK and stages the matching source archive, SHA-256 checksums, and upstream attribution files. If publishing the Windows and macOS binaries, publish the corresponding Qt source beside them. Keep the included license texts, attribution, and library replacement instructions. The Ubuntu package links to the Qt packages installed by the distribution. [Qt redistribution obligations](https://www.qt.io/development/open-source-lgpl-obligations)

The current Windows and macOS previews bundle Qt 6.12.0. Before publishing binaries, update the SDK and matching source to a build that includes the Qt Base security fixes published for the 6.12 series, then rebuild and retest the packages. [Qt Base security patches](https://download.qt.io/official_releases/qt/6.12/)

For a public macOS download, use a Developer ID signature and Apple's notarization service, then verify the downloaded package on a clean Mac. The artifact workflow stages ad hoc builds for testing. [Apple notarization guide](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)

## Before making a release public

Confirm the repository and license with the maintainer, and review Git history, package contents, and screenshots for private information. Check the downloaded ZIP, DMG, and DEB on the target architectures and publish only the verified files and their checksums. Enable private vulnerability reporting before directing users to it. See [Security](../SECURITY.md).
