# Release guide

Build and test the release candidate from a clean checkout. The repository's **Build and test** workflow checks the desktop application on Windows, macOS, and Ubuntu. In GitHub Actions, select **Prepare release artifacts**, choose the candidate branch or tag, and run the workflow. Download the packages and corresponding Qt source from the workflow run.

| Package | Target | Notes |
| --- | --- | --- |
| ZIP | Windows 10 (1809 or later) and Windows 11, x64 | Extract before running `bin/FramelineGUI.exe`. The preview is unsigned. |
| DMG | macOS 14.4+, Apple Silicon and Intel | The preview is ad hoc signed. Validate a quarantined download before public distribution. |
| DEB | Ubuntu 24.04, amd64 | Uses system Qt packages. Capture works in X11 or through a compatible Wayland portal. |

Use a fresh profile and sample screenshots for these checks:

1. Verify full-screen capture, region selection, save, copy, and keyboard navigation before configuring a library.
2. Capture a sample screenshot with **Sync across devices** set to **Local only** and open **Library** while the editor is still open. Check that the capture appears. Quit and reopen Frameline, then check that the capture is still available.
3. Under **Settings → Sync across devices → Host here**, select **Create shared library**. Enable **Access from other devices**, apply sharing, copy the code and use **Connect** on another computer signed in to the same Tailscale account. Verify browsing and automatic upload in **Shared**. Disconnect Tailscale, check that local captures remain available and uploads wait in the queue, then reconnect and confirm they arrive. Turn sharing off and verify local history again.
4. Enable **Start Frameline when I sign in**, sign out and back in, and verify Frameline starts in the tray and serves the library. Launch the app again and check that the running instance opens its window. Disable the setting and verify it stays off at the next login.
5. Connect to a disposable standalone library over SSH and verify browsing and automatic upload. Confirm that upgrading preserves settings and queued captures, and that queued captures stay with their original library when you switch destinations.

## Windows and macOS dependencies

The release workflow builds shared Qt Base 6.12.0 from the official source with Qt's patches for CVE-2026-105924, CVE-2026-105943, and CVE-2026-116337. It verifies the source and patches by SHA-256 before building. [Qt Base security patches](https://download.qt.io/official_releases/qt/6.12/)

Publish the corresponding Qt source bundle alongside the Windows and macOS packages. It contains the upstream archive, the applied patches, their checksums, and the build recipe. Keep the included license texts, attribution, and library replacement instructions. [Qt redistribution obligations](https://www.qt.io/development/open-source-lgpl-obligations)

For a public macOS download, use a Developer ID signature and Apple's notarization service, then verify the downloaded package on a clean Mac. The artifact workflow stages ad hoc builds for testing. [Apple notarization guide](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution)

## Ubuntu dependencies

The DEB uses the Qt libraries from Ubuntu 24.04. Install Ubuntu's security updates to receive fixes for these libraries. Canonical tracks the status of [CVE-2026-105924](https://ubuntu.com/security/CVE-2026-105924) and [CVE-2026-116337](https://ubuntu.com/security/CVE-2026-116337) for its `qt6-base` package.

## Before making a release public

Review Git history, package contents, and screenshots for private information. Check the downloaded ZIP, DMG, and DEB on the target architectures and publish only the verified files and their checksums. Enable private vulnerability reporting before directing users to it. See [Security](../SECURITY.md).
