#include "application.h"

#include "libraryclient.h"
#include "libraryserver.h"
#include "librarywindow.h"
#include "pngexport.h"
#include "regionselector.h"
#include "settingswindow.h"
#include "singleinstance.h"
#include "tailscale.h"
#include "platform/autostart.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
#elif defined(Q_OS_LINUX)
#include <QBuffer>
#endif

namespace {

// Time for the compositor to remove FreezeCut's own windows from the screen before capturing.
constexpr int compositorDelayMs = 180;

void bringToFront(QWidget &window)
{
    window.showNormal();
#ifdef Q_OS_WIN
    // A hidden launch can override the first native ShowWindow call in the process.
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        const auto handle = reinterpret_cast<HWND>(window.winId());
        if (!IsWindowVisible(handle))
            ShowWindow(handle, SW_SHOWNORMAL);
    }
#endif
    window.raise();
    window.activateWindow();
}

#ifdef Q_OS_LINUX
bool copyToWaylandClipboard(const QImage &image)
{
    QBuffer png;
    if (!png.open(QIODevice::WriteOnly) || !image.save(&png, "PNG"))
        return false;

    // wl-copy can set the selection without focus on FreezeCut and keep it after exit.
    QProcess copy;
    copy.setStandardOutputFile(QProcess::nullDevice());
    copy.setStandardErrorFile(QProcess::nullDevice());
    copy.start(QStringLiteral("wl-copy"), {QStringLiteral("--type"), QStringLiteral("image/png")});
    if (!copy.waitForStarted(500))
        return false;
    copy.write(png.data());
    copy.closeWriteChannel();
    if (!copy.waitForFinished(1000)) {
        copy.kill();
        copy.waitForFinished();
        return false;
    }
    return copy.exitStatus() == QProcess::NormalExit && copy.exitCode() == 0;
}
#endif

void copyScreenshotToClipboard(const QImage &image)
{
#ifdef Q_OS_LINUX
    if (ScreenCapture::usesScreenshotPortal() && copyToWaylandClipboard(image))
        return;
#endif
    QApplication::clipboard()->setImage(image);
}

} // namespace

FreezeCutApplication::FreezeCutApplication(LaunchOptions options)
    : options_(std::move(options))
{
    const QString directory = defaultSettingsDirectory();
    settings_ = loadDesktopSettings(directory, &settingsError_);
    history_ = std::make_unique<LibraryClient>(directory, settings_.hostedLibrary.directory);
    libraryServer_ = std::make_unique<LibraryServer>();
    tailscale_ = std::make_unique<TailscaleMonitor>();
    libraryServer_->setPeerAuthorizer([this](const QString &local, const QString &peer) {
        return tailscale_->status().sourceForPeer(peer) == local;
    });
    connect(tailscale_.get(), &TailscaleMonitor::statusChanged, this, [this](const TailscaleStatus &status) {
        if (settingsWindow_)
            settingsWindow_->setTailscaleStatus(status);
    });
    connect(tailscale_.get(), &TailscaleMonitor::refreshed, this, [this] { refreshHostedSharing(); });
    libraryClient_ = std::make_unique<LibraryClient>(settings_.library, directory);
    connectLibraryClient();
    connect(history_.get(), &LibraryClient::syncStatusChanged, this, [this](int, const QString &error) {
        if (historyError_ != error) {
            historyError_ = error;
            refreshLibraryStatus(libraryClient_->pendingCount(), syncError_);
        }
    });
    connect(qApp, &QApplication::aboutToQuit, this, [this] {
        archivePreview();
        flushLibrary();
    });
    connect(qApp, &QApplication::lastWindowClosed, this, [this] {
        QTimer::singleShot(0, this, [this] {
            if (!tray_.isVisible() && !busy_ && !settingsWindow_
                && (!editor_ || !editor_->isVisible()) && (!library_ || !library_->isVisible())
                && (isDesktop() || previewOpened_))
                exitAfterLibrarySync();
        });
    });
    connect(&shortcuts_, &GlobalShortcuts::captureRequested, this, [this](bool region) { beginCapture(region); });
    connect(&shortcuts_, &GlobalShortcuts::statusChanged, this, &FreezeCutApplication::refreshShortcuts);
    connect(&capture_, &ScreenCapture::failed, this, &FreezeCutApplication::failCapture);
    connect(&capture_, &ScreenCapture::portalPermissionRequired, this, &FreezeCutApplication::requestPortalPermission);
    connect(&capture_, &ScreenCapture::cancelled, this, [this] { finishCapture({}); });
    connect(&capture_, &ScreenCapture::captured, this, [this](const QList<CaptureFrame> &frames) {
        if (std::exchange(portalAuthorizing_, false)) {
            editor_->hide();
            captureAfterHiding(true);
            return;
        }
        bool selectRegion = region_;
#ifdef FREEZECUT_LINUX_SNAP
        // The system picker already chose what to capture.
        selectRegion = selectRegion && !interactivePortalCapture_;
#endif
        if (!selectRegion || frames.isEmpty()) {
            finishCapture(frames.isEmpty() ? QImage{} : frames.first().image);
            return;
        }
        RegionSelector::start(frames, [this](QImage image, const std::function<void()> &dismiss) {
            finishCapture(std::move(image), dismiss);
        });
    });
}

FreezeCutApplication::~FreezeCutApplication() = default;

void FreezeCutApplication::connectLibraryClient()
{
    connect(libraryClient_.get(), &LibraryClient::syncStatusChanged, this, &FreezeCutApplication::refreshLibraryStatus);
}

bool FreezeCutApplication::isDesktop() const
{
    return options_.mode == LaunchOptions::Mode::Desktop || options_.mode == LaunchOptions::Mode::Library
        || options_.mode == LaunchOptions::Mode::Settings;
}

void FreezeCutApplication::start()
{
    // The capture overlay can briefly be the last visible window. Closing the final editor
    // should also give the Library time to save the capture.
    QApplication::setQuitOnLastWindowClosed(false);
    if (isDesktop()) {
        Autostart::refreshAtStartup();
        startHostedLibrary(settings_.hostedLibrary, &hostedLibraryError_);
        if (settings_.library.transport == LibraryTransport::Local
            && settings_.hostedLibrary.enabled && settings_.library.token == settings_.hostedLibrary.token)
            libraryClient_->setConnectionError(hostedLibraryError_);
        if (settings_.hostedLibrary.share)
            tailscale_->refresh();
    }
    history_->start();
    libraryClient_->start();
    if (!hostedLibraryError_.isEmpty())
        refreshLibraryStatus(libraryClient_->pendingCount(), hostedLibraryError_);
    if (!settingsError_.isEmpty()) {
        if (isDesktop())
            refreshLibraryStatus(0, settingsError_);
        else
            QTextStream(stderr) << settingsError_ << Qt::endl;
    }
    if (!isDesktop()) {
        QTimer::singleShot(0, this, [this] { beginCapture(options_.mode == LaunchOptions::Mode::Region); });
        return;
    }

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_.setIcon(QApplication::windowIcon());
        tray_.setToolTip(libraryMessage_.isEmpty() ? QStringLiteral("FreezeCut") : libraryMessage_);
        trayMenu_ = std::make_unique<QMenu>();
        // Capturing is what the tray is for, so it comes first.
        trayMenu_->addAction(QStringLiteral("Capture Region"), this, [this] { beginCapture(true, true); });
        trayMenu_->addAction(QStringLiteral("Capture Screen"), this, [this] { beginCapture(false, true); });
        trayMenu_->addSeparator();
        trayMenu_->addAction(QStringLiteral("Open FreezeCut"), this, &FreezeCutApplication::showEditor);
        trayMenu_->addAction(QStringLiteral("Library"), this, &FreezeCutApplication::showLibrary);
        trayMenu_->addAction(QStringLiteral("Settings…"), this, &FreezeCutApplication::showSettings);
#if defined(FREEZECUT_MAC_APP_STORE) || defined(FREEZECUT_WINDOWS_STORE) || defined(FREEZECUT_LINUX_SNAP)
        trayMenu_->addAction(QStringLiteral("Privacy Policy"), this, [] {
            QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/Xopoko/FreezeCut/blob/main/PRIVACY.md")));
        });
        trayMenu_->addAction(QStringLiteral("Open Source Licenses…"), this, [] {
            QMessageBox notice(QMessageBox::Information, QStringLiteral("Open Source Licenses"),
                QStringLiteral("FreezeCut uses Qt Base under the GNU Lesser General Public License version 3."),
                QMessageBox::Close);
            const auto directory = QDir(QCoreApplication::applicationDirPath());
#if defined(FREEZECUT_WINDOWS_STORE) || defined(FREEZECUT_LINUX_SNAP)
#ifdef FREEZECUT_LINUX_SNAP
            const QDir notices(directory.absoluteFilePath(QStringLiteral("../share/doc/freezecut/Qt-notices")));
#else
            // The package can read its bundled notices even when Explorer cannot open WindowsApps.
            const QDir notices(directory.absoluteFilePath(QStringLiteral("../Qt-notices")));
#endif
            QStringList licenseText;
            for (const auto *fileName : {"README.txt", "THIRD-PARTY.txt", "upstream/LICENSES/LGPL-3.0-only.txt",
                                        "upstream/LICENSES/GPL-3.0-only.txt"}) {
                QFile file(notices.filePath(QString::fromLatin1(fileName)));
                if (file.open(QIODevice::ReadOnly | QIODevice::Text))
                    licenseText.append(QString::fromUtf8(file.readAll()));
            }
            notice.setDetailedText(licenseText.join(QStringLiteral("\n\n")));
#else
            auto *licenses = notice.addButton(QStringLiteral("View Licenses"), QMessageBox::ActionRole);
#endif
            auto *source = notice.addButton(QStringLiteral("Get Source Code"), QMessageBox::ActionRole);
            notice.exec();
#ifdef FREEZECUT_MAC_APP_STORE
            if (notice.clickedButton() == licenses) {
                QDesktopServices::openUrl(QUrl::fromLocalFile(directory.absoluteFilePath(
                    QStringLiteral("../Resources/Qt-notices"))));
                return;
            }
#endif
            if (notice.clickedButton() == source)
                QDesktopServices::openUrl(QUrl(QStringLiteral(FREEZECUT_STORE_SOURCE_URL)));
        });
#endif
        trayMenu_->addSeparator();
        trayMenu_->addAction(QStringLiteral("Quit FreezeCut"), this, &FreezeCutApplication::quitDesktop);
        tray_.setContextMenu(trayMenu_.get());
        connect(&tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger) {
                showEditor();
            }
        });
        tray_.show();
    }

    shortcuts_.start(settings_.useSystemRegionShortcut ? RegionShortcutPreset::System : RegionShortcutPreset::Standard);
    if (options_.mode == LaunchOptions::Mode::Settings) {
        showSettings();
        if (!tray_.isVisible())
            showEditor();
    } else if (options_.mode == LaunchOptions::Mode::Library) {
        showLibrary();
    } else if (!options_.trayOnly || !tray_.isVisible()) {
        showEditor();
    }
}

void FreezeCutApplication::activate(LaunchOptions::Mode mode)
{
    if (settingsWindow_) {
        bringToFront(*settingsWindow_);
    } else if (mode == LaunchOptions::Mode::Settings) {
        showSettings();
    } else if (mode == LaunchOptions::Mode::Library) {
        showLibrary();
    } else {
        showEditor();
    }
}

bool FreezeCutApplication::startHostedLibrary(const HostedLibrarySettings &settings, QString *error)
{
    libraryServer_->stop();
    if (error)
        error->clear();
    hostedSharingError_.clear();
    if (!settings.enabled)
        return true;
    return libraryServer_->start(settings.directory, settings.token, quint16(settings.port), error);
}

void FreezeCutApplication::refreshHostedSharing()
{
    hostedSharingError_.clear();
    QString address;
    if (libraryServer_->port() && settings_.hostedLibrary.share) {
        const TailscaleStatus status = tailscale_->status();
        if (status.state == TailscaleState::Ready)
            address = status.localAddress;
        else
            hostedSharingError_ = status.message;
    }
    if (libraryServer_->port() && libraryServer_->shareAddress() != address) {
        QString error;
        if (!libraryServer_->setShareAddress(address, &error))
            hostedSharingError_ = error;
    }
    if (settingsWindow_)
        settingsWindow_->setHostedStatus(libraryServer_->port() != 0, libraryServer_->shareAddress(),
                                        hostedLibraryError_.isEmpty() ? hostedSharingError_ : hostedLibraryError_);
}

EditorWindow &FreezeCutApplication::editor()
{
    if (!editor_) {
        editor_ = std::make_unique<EditorWindow>();
        editor_->setShortcuts(fullShortcut_, regionShortcut_, shortcutMessage_);
        editor_->setLibraryStatus(libraryMessage_);
        connect(editor_.get(), &EditorWindow::fullCaptureRequested, this, [this] { beginCapture(false); });
        connect(editor_.get(), &EditorWindow::regionCaptureRequested, this, [this] { beginCapture(true); });
        connect(editor_.get(), &EditorWindow::libraryRequested, this, &FreezeCutApplication::showLibrary);
        connect(editor_.get(), &EditorWindow::settingsRequested, this, &FreezeCutApplication::showSettings);
        connect(editor_.get(), &EditorWindow::closeAccepted, this, &FreezeCutApplication::archivePreview);
    }
    return *editor_;
}

void FreezeCutApplication::showEditor()
{
    if (busy_ || quitting_) {
        return;
    }
    bringToFront(editor());
}

void FreezeCutApplication::showLibrary()
{
    if (busy_ || quitting_) {
        return;
    }
    if (!library_) {
        library_ = std::make_unique<LibraryWindow>(*history_, nullptr,
                                                  libraryClient_->isConfigured() ? libraryClient_.get() : nullptr);
        connect(library_.get(), &LibraryWindow::settingsRequested, this, &FreezeCutApplication::showSettings,
                Qt::QueuedConnection);
    }
    bringToFront(*library_);
}

void FreezeCutApplication::showSettings()
{
    if (busy_ || quitting_ || settingsWindow_)
        return;
    if (!isDesktop()) {
        if (SingleInstance::requestSettings())
            return;
        QProcess desktop;
        QString executable = QCoreApplication::applicationFilePath();
#ifdef Q_OS_WIN
        executable = QFileInfo(executable).dir().filePath(QStringLiteral("FreezeCutGUI.exe"));
        desktop.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
            arguments->flags |= CREATE_NO_WINDOW;
        });
#endif
        desktop.setProgram(executable);
        desktop.setArguments({QStringLiteral("settings")});
        if (!desktop.startDetached())
            QMessageBox::warning(editor_.get(), QStringLiteral("FreezeCut settings"),
                                 QStringLiteral("Open FreezeCut from your applications to change settings."));
        return;
    }

    QString settingsError;
    const AppSettings saved = loadDesktopSettings(defaultSettingsDirectory(), &settingsError);
    settingsWindow_ = std::make_unique<SettingsWindow>(saved);
    settingsWindow_->setTailscaleStatus(tailscale_->status());
    refreshHostedSharing();
    connect(settingsWindow_.get(), &SettingsWindow::refreshTailscaleRequested, tailscale_.get(), &TailscaleMonitor::refresh);
    connect(settingsWindow_.get(), &SettingsWindow::saveRequested, this, &FreezeCutApplication::saveWindowSettings);
    connect(settingsWindow_.get(), &SettingsWindow::testRequested, this, [this](const LibrarySettings &settings) {
        testClient_ = std::make_unique<LibraryClient>(settings);
        testClient_->list({}, settingsWindow_.get(), [this](QList<LibraryItem>, const QString &, const QString &error) {
            settingsWindow_->setTestResult(error);
        });
    });
    if (!settingsError.isEmpty()) {
        QMessageBox::warning(settingsWindow_.get(), QStringLiteral("FreezeCut settings"),
                             settingsError + QStringLiteral("\n\nSave new settings to replace the unreadable configuration."));
    }
    if (saved.hostedLibrary.share || saved.library.transport == LibraryTransport::Tailscale)
        tailscale_->refresh();
    settingsWindow_->exec();
    settingsWindow_.reset();
    testClient_.reset();
    if (std::exchange(restoreLibraryAfterSettings_, false))
        showLibrary();
}

void FreezeCutApplication::saveWindowSettings(AppSettings updated, bool autostart)
{
    if (!settingsWindow_)
        return;
    const auto fail = [this](const QString &error) { settingsWindow_->setSaveResult(error, settings_); };
    QString error;
    const bool editAutostart = settingsWindow_->autostartEditable();
    const Autostart::State previousAutostart = editAutostart ? Autostart::snapshot(&error) : Autostart::State{};
    if (!error.isEmpty()) {
        fail(error);
        return;
    }
    if (updated.library != settings_.library && !flushLibrary()) {
        fail(QStringLiteral("Could not save the pending screenshots on this computer. Try again after the queue error clears."));
        return;
    }
    auto previousHost = settings_.hostedLibrary;
    previousHost.share = updated.hostedLibrary.share;
    const bool hostingChanged = updated.hostedLibrary != previousHost
        || (updated.hostedLibrary.enabled && !libraryServer_->port());
    const auto restoreHosting = [this, hostingChanged] {
        if (hostingChanged) {
            startHostedLibrary(settings_.hostedLibrary, &hostedLibraryError_);
            refreshHostedSharing();
        }
    };
    if (hostingChanged && !startHostedLibrary(updated.hostedLibrary, &error)) {
        // A new library may choose a free port without invalidating any existing invitations.
        const bool newLibrary = settings_.hostedLibrary.token.isEmpty();
        const bool started = newLibrary && updated.hostedLibrary.enabled
            && libraryServer_->start(updated.hostedLibrary.directory, updated.hostedLibrary.token, 0, &error);
        if (!started) {
            restoreHosting();
            fail(error);
            return;
        }
        updated.hostedLibrary.port = libraryServer_->port();
        if (updated.library.transport == LibraryTransport::Local
            && updated.library.token == updated.hostedLibrary.token)
            updated.library.port = updated.hostedLibrary.port;
    }
    const bool changeAutostart = editAutostart && (autostart || previousAutostart.isEnabled());
    if (changeAutostart && !Autostart::setEnabled(autostart, &error)) {
        QString rollbackError;
        if (!Autostart::restore(previousAutostart, &rollbackError))
            error += QStringLiteral("\n") + rollbackError;
        restoreHosting();
        fail(error);
        return;
    }
    if (!saveDesktopSettings(updated, defaultSettingsDirectory(), &error)) {
        if (changeAutostart) {
            QString rollbackError;
            if (!Autostart::restore(previousAutostart, &rollbackError))
                error += QStringLiteral("\n") + rollbackError;
        }
        restoreHosting();
        fail(error);
        return;
    }

    const bool libraryChanged = updated.library != settings_.library;
    const bool destinationChanged = updated.library.storageKey() != settings_.library.storageKey();
    const bool shortcutChanged = updated.useSystemRegionShortcut != settings_.useSystemRegionShortcut;
    const bool libraryWasVisible = library_ && library_->isVisible();
    const bool keepPreviewLocal = previewCapture_.capturedAt.isValid()
        && (destinationChanged || updated.library.autoArchive != settings_.library.autoArchive);
    if (keepPreviewLocal)
        previewCapture_ = {};
    settings_ = updated;
    history_->setHostedHistoryDirectory(settings_.hostedLibrary.directory);
    const bool hadSettingsError = !settingsError_.isEmpty() || !hostedLibraryError_.isEmpty();
    hostedLibraryError_.clear();
    settingsError_.clear();
    if (libraryChanged) {
        library_.reset();
        libraryClient_ = std::make_unique<LibraryClient>(settings_.library);
        connectLibraryClient();
        refreshLibraryStatus(0, {});
        libraryClient_->start();
        restoreLibraryAfterSettings_ = restoreLibraryAfterSettings_ || libraryWasVisible;
    } else if (hadSettingsError) {
        libraryClient_->setConnectionError({});
        refreshLibraryStatus(0, {});
    }
    if (shortcutChanged || keepPreviewLocal) {
        QString message;
        if (shortcutChanged)
            message = QStringLiteral("Quit and reopen FreezeCut to apply the shortcut change.");
        if (keepPreviewLocal) {
            if (!message.isEmpty())
                message += QStringLiteral("\n\n");
            message += QStringLiteral("The screenshot already open will stay in the local Library.");
        }
        QMessageBox::information(settingsWindow_.get(), QStringLiteral("Settings saved"), message);
    }
    settingsWindow_->setSaveResult({}, settings_);
    refreshHostedSharing();
    if (settings_.hostedLibrary.share)
        tailscale_->refresh();
}

void FreezeCutApplication::quitDesktop()
{
    if (quitting_ || settingsWindow_) {
        return;
    }
    if (busy_) {
        exitAfterCapture_ = true;
        return;
    }
    if (editor_ && editor_->hasUnsavedChanges()) {
        showEditor();
        if (!editor_->confirmReplace()) {
            return;
        }
    }
    archivePreview();
    exitAfterLibrarySync();
}

void FreezeCutApplication::beginCapture(bool region, bool fromTrayMenu)
{
    if (busy_ || quitting_ || settingsWindow_) {
        return;
    }

    busy_ = true;
    region_ = region;
    portalAuthorizationAttempted_ = false;
#ifdef FREEZECUT_LINUX_SNAP
    interactivePortalCapture_ = false;
#endif
    restoreEditor_ = editor_ && editor_->isVisible();
    restoreLibrary_ = library_ && library_->isVisible() && !library_->isMinimized();
    if (editor_ && editor_->hasUnsavedChanges()) {
        bringToFront(*editor_);
        if (!editor_->confirmReplace()) {
            busy_ = false;
            if (std::exchange(exitAfterCapture_, false)) {
                quitDesktop();
            }
            return;
        }
    }

    const bool surfaceWasVisible = fromTrayMenu || (trayMenu_ && trayMenu_->isVisible())
        || restoreLibrary_ || (editor_ && editor_->isVisible() && !editor_->isMinimized());
    if (editor_) {
        editor_->hide();
    }
    if (trayMenu_) {
        trayMenu_->hide();
    }
    if (library_) {
        library_->hide();
    }
    captureAfterHiding(surfaceWasVisible);
}

void FreezeCutApplication::captureAfterHiding(bool surfaceWasVisible)
{
    const auto capture = [this] { capture_.capture(region_); };
#ifdef Q_OS_MACOS
    // ScreenCaptureKit filters out FreezeCut even while its windows are being hidden.
    Q_UNUSED(surfaceWasVisible);
    QTimer::singleShot(0, this, capture);
#else
    if (!surfaceWasVisible) {
        QTimer::singleShot(0, this, capture);
        return;
    }
#ifdef Q_OS_WIN
    QTimer::singleShot(0, this, [this, capture] {
        if (SUCCEEDED(DwmFlush())) {
            capture();
        } else {
            QTimer::singleShot(compositorDelayMs, this, capture);
        }
    });
#else
    QTimer::singleShot(compositorDelayMs, this, capture);
#endif
#endif
}

void FreezeCutApplication::finishCapture(QImage image, const std::function<void()> &dismissSelection)
{
    busy_ = false;
    portalAuthorizing_ = false;
    const bool shouldQuit = std::exchange(exitAfterCapture_, false);
    if (image.isNull()) {
        if (isDesktop() || previewOpened_) {
            restoreWindows();
        } else {
            QApplication::exit(2);
        }
        if (shouldQuit) {
            quitDesktop();
        }
        return;
    }

    const bool showPreview = options_.preview && settings_.showEditorAfterCapture;
    if (!showPreview && dismissSelection)
        dismissSelection();

    const QString kind = region_ ? QStringLiteral("region") : QStringLiteral("full");
    const QDateTime capturedAt = QDateTime::currentDateTimeUtc();
    const QString historyId = history_->enqueue(image, kind, capturedAt);
    archivePreview();
    if (showPreview) {
        restoreEditor_ = false;
        restoreLibrary_ = false;
        image.setDevicePixelRatio(1);
        editor().setCapture(image);
        previewCapture_ = {kind, capturedAt, historyId};
        previewOpened_ = true;
        // The selection still owns the foreground when the editor is first shown.
        showEditor();
        if (dismissSelection)
            dismissSelection();
    }
    if (settings_.autoCopyScreenshot)
        copyScreenshotToClipboard(image);

    QString outputError;
    if (!options_.output.isEmpty()) {
        QString error;
        if (!savePng(image, options_.output, &error)) {
            outputError = QStringLiteral("Could not save the PNG to %1. %2").arg(options_.output, error);
            QTextStream(stderr) << outputError << Qt::endl;
            exitCode_ = 1;
        }
        options_.output.clear();
    }

    if (!showPreview) {
        libraryClient_->enqueue(std::move(image), kind, capturedAt, historyId);
        if (isDesktop()) {
            if (editor_)
                editor_->setCapture({});
            if (tray_.isVisible())
                restoreEditor_ = false;
            restoreWindows();
            if (shouldQuit)
                quitDesktop();
        } else {
            exitAfterLibrarySync();
        }
        return;
    }

    if (!outputError.isEmpty()) {
        QMessageBox dialog(QMessageBox::Warning, QStringLiteral("Could not save screenshot"), outputError,
                           QMessageBox::Ok, editor_.get());
        dialog.setTextFormat(Qt::PlainText);
        dialog.exec();
    }
    if (shouldQuit) {
        quitDesktop();
    }
}

void FreezeCutApplication::failCapture(const QString &message)
{
    busy_ = false;
    portalAuthorizing_ = false;
    const bool shouldQuit = std::exchange(exitAfterCapture_, false);
    if (!isDesktop() && !previewOpened_) {
        QTextStream(stderr) << message << Qt::endl;
        if (options_.preview && settings_.showEditorAfterCapture) {
            QMessageBox::warning(nullptr, QStringLiteral("FreezeCut"), message);
        }
        exitCode_ = 1;
        exitAfterLibrarySync();
        return;
    }

    restoreWindows();
    showEditor();
    QMessageBox::warning(editor_.get(), QStringLiteral("Capture Failed"), message);
    if (shouldQuit) {
        quitDesktop();
    }
}

void FreezeCutApplication::requestPortalPermission(const QString &message)
{
#ifdef FREEZECUT_LINUX_SNAP
    // GNOME may refuse the passive access dialog when the Snap's portal identity
    // differs from its desktop file. The system picker grants access per capture.
    if (std::exchange(interactivePortalCapture_, true)) {
        failCapture(message);
        return;
    }
    capture_.capture(region_, nullptr, true);
#else
    if (!options_.preview || std::exchange(portalAuthorizationAttempted_, true)) {
        failCapture(message);
        return;
    }

    auto &window = editor();
    bringToFront(window);
    QMessageBox dialog(QMessageBox::Question, QStringLiteral("Screen Capture Permission"), message,
                       QMessageBox::NoButton, &window);
    dialog.setInformativeText(QStringLiteral("FreezeCut will request access with this window open. "
                                              "Once approved, it will hide the window and take a new screenshot."));
    const auto *retry = dialog.addButton(QStringLiteral("Request Access"), QMessageBox::AcceptRole);
    dialog.addButton(QMessageBox::Cancel);
    dialog.exec();
    if (dialog.clickedButton() != retry) {
        window.hide();
        finishCapture({});
        return;
    }

    portalAuthorizing_ = true;
    QTimer::singleShot(compositorDelayMs, this, [this] { capture_.capture(region_, editor_.get()); });
#endif
}

void FreezeCutApplication::restoreWindows()
{
    if (std::exchange(restoreEditor_, false)) {
        showEditor();
    }
    if (std::exchange(restoreLibrary_, false)) {
        showLibrary();
    }
}

void FreezeCutApplication::archivePreview()
{
    if (!previewCapture_.capturedAt.isValid()) {
        return;
    }
    const auto capture = std::exchange(previewCapture_, {});
    if (editor_) {
        libraryClient_->enqueue(editor_->currentImage(), capture.kind, capture.capturedAt, capture.id);
    }
}

bool FreezeCutApplication::flushLibrary()
{
    const bool historySaved = history_->flushPendingWrites();
    const bool queueSaved = libraryClient_->flushPendingWrites();
    if (historySaved && queueSaved) {
        libraryFlushFailed_ = false;
        return true;
    }
    const QString error = historySaved ? QStringLiteral("Could not save a screenshot to the upload queue.")
                                       : QStringLiteral("Could not save a screenshot to the local Library.");
    if (!isDesktop() && !libraryFlushFailed_) {
        QTextStream(stderr) << error << Qt::endl;
    }
    libraryFlushFailed_ = true;
    if (historySaved)
        refreshLibraryStatus(libraryClient_->pendingCount(), error);
    return false;
}

void FreezeCutApplication::exitAfterLibrarySync()
{
    if (quitting_) {
        return;
    }
    if (!flushLibrary()) {
        if (isDesktop() || previewOpened_) {
            showEditor();
            return;
        }
        exitCode_ = 1;
    }
    quitting_ = true;

    const auto exit = [this] { QApplication::exit(exitCode_); };
    if (libraryClient_->pendingCount() == 0) {
        QTimer::singleShot(0, this, exit);
        return;
    }

    connect(libraryClient_.get(), &LibraryClient::syncStatusChanged, this,
            [exit](int pending, const QString &) {
                if (pending == 0) {
                    exit();
                }
            });
    constexpr int syncWaitMs = 1800;
    QTimer::singleShot(syncWaitMs, this, exit);
}

void FreezeCutApplication::refreshLibraryStatus(int pending, const QString &error)
{
    syncError_ = error;
    const QString message = !historyError_.isEmpty() ? historyError_
        : hostedLibraryError_.isEmpty() ? syncError_ : hostedLibraryError_;
    if (message.isEmpty()) {
        libraryMessage_.clear();
    } else if (pending > 0 && historyError_.isEmpty()) {
        libraryMessage_ = QStringLiteral("Library (%1 pending): %2").arg(pending).arg(message);
    } else {
        libraryMessage_ = QStringLiteral("Library: %1").arg(message);
    }
    tray_.setToolTip(libraryMessage_.isEmpty() ? QStringLiteral("FreezeCut") : libraryMessage_);
    if (editor_) {
        editor_->setLibraryStatus(libraryMessage_);
    }
}

void FreezeCutApplication::refreshShortcuts(bool full, bool region, const QString &message)
{
    fullShortcut_ = full ? shortcuts_.description(false) : QString{};
    regionShortcut_ = region ? shortcuts_.description(true) : QString{};
    shortcutMessage_ = message;
    if (editor_) {
        editor_->setShortcuts(fullShortcut_, regionShortcut_, shortcutMessage_);
    }
}
