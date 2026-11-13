#include "application.h"

#include "libraryclient.h"
#include "librarywindow.h"
#include "pngexport.h"
#include "regionselector.h"
#include "settingswindow.h"

#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QTimer>

#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
#endif

namespace {

// Time for the compositor to remove ScreenFrame's own windows from the screen before capturing.
constexpr int compositorDelayMs = 180;

void bringToFront(QWidget &window)
{
    window.showNormal();
    window.raise();
    window.activateWindow();
}

} // namespace

ScreenFrameApplication::ScreenFrameApplication(LaunchOptions options)
    : options_(std::move(options))
{
    settings_ = loadSettings(defaultSettingsDirectory(), &settingsError_);
    libraryClient_ = std::make_unique<LibraryClient>(settings_.library);
    connectLibraryClient();
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
    connect(&shortcuts_, &GlobalShortcuts::statusChanged, this, &ScreenFrameApplication::refreshShortcuts);
    connect(&capture_, &ScreenCapture::failed, this, &ScreenFrameApplication::failCapture);
    connect(&capture_, &ScreenCapture::portalPermissionRequired, this, &ScreenFrameApplication::requestPortalPermission);
    connect(&capture_, &ScreenCapture::cancelled, this, [this] { finishCapture({}); });
    connect(&capture_, &ScreenCapture::captured, this, [this](const QList<CaptureFrame> &frames) {
        if (std::exchange(portalAuthorizing_, false)) {
            editor_->hide();
            captureAfterHiding(true);
            return;
        }
        if (!region_ || frames.isEmpty()) {
            finishCapture(frames.isEmpty() ? QImage{} : frames.first().image);
            return;
        }
        RegionSelector::start(frames, [this](QImage image) { finishCapture(std::move(image)); });
    });
}

ScreenFrameApplication::~ScreenFrameApplication() = default;

void ScreenFrameApplication::connectLibraryClient()
{
    connect(libraryClient_.get(), &LibraryClient::syncStatusChanged, this, &ScreenFrameApplication::refreshLibraryStatus);
}

bool ScreenFrameApplication::isDesktop() const
{
    return options_.mode == LaunchOptions::Mode::Desktop || options_.mode == LaunchOptions::Mode::Library;
}

void ScreenFrameApplication::start()
{
    // The capture overlay can briefly be the last visible window. Closing the final editor
    // should also give the local Library queue time to finish writing.
    QApplication::setQuitOnLastWindowClosed(false);
    libraryClient_->start();
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
        tray_.setToolTip(libraryMessage_.isEmpty() ? QStringLiteral("ScreenFrame") : libraryMessage_);
        trayMenu_ = std::make_unique<QMenu>();
        // Capturing is what the tray is for, so it comes first.
        trayMenu_->addAction(QStringLiteral("Capture Region"), this, [this] { beginCapture(true, true); });
        trayMenu_->addAction(QStringLiteral("Capture Screen"), this, [this] { beginCapture(false, true); });
        trayMenu_->addSeparator();
        trayMenu_->addAction(QStringLiteral("Open ScreenFrame"), this, &ScreenFrameApplication::showEditor);
        trayMenu_->addAction(QStringLiteral("Library"), this, &ScreenFrameApplication::showLibrary);
        trayMenu_->addAction(QStringLiteral("Settings…"), this, &ScreenFrameApplication::showSettings);
        trayMenu_->addSeparator();
        trayMenu_->addAction(QStringLiteral("Quit ScreenFrame"), this, &ScreenFrameApplication::quitDesktop);
        tray_.setContextMenu(trayMenu_.get());
        connect(&tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger) {
                showEditor();
            }
        });
        tray_.show();
    }

    shortcuts_.start(settings_.useSystemRegionShortcut ? RegionShortcutPreset::System : RegionShortcutPreset::Standard);
    if (options_.mode == LaunchOptions::Mode::Library) {
        showLibrary();
    } else if (!options_.trayOnly || !tray_.isVisible()) {
        showEditor();
    }
}

EditorWindow &ScreenFrameApplication::editor()
{
    if (!editor_) {
        editor_ = std::make_unique<EditorWindow>();
        editor_->setShortcuts(fullShortcut_, regionShortcut_, shortcutMessage_);
        editor_->setLibraryStatus(libraryMessage_);
        connect(editor_.get(), &EditorWindow::fullCaptureRequested, this, [this] { beginCapture(false); });
        connect(editor_.get(), &EditorWindow::regionCaptureRequested, this, [this] { beginCapture(true); });
        connect(editor_.get(), &EditorWindow::libraryRequested, this, &ScreenFrameApplication::showLibrary);
        connect(editor_.get(), &EditorWindow::settingsRequested, this, &ScreenFrameApplication::showSettings);
        connect(editor_.get(), &EditorWindow::closeAccepted, this, &ScreenFrameApplication::archivePreview);
    }
    return *editor_;
}

void ScreenFrameApplication::showEditor()
{
    if (busy_ || quitting_) {
        return;
    }
    bringToFront(editor());
}

void ScreenFrameApplication::showLibrary()
{
    if (busy_ || quitting_) {
        return;
    }
    if (!library_) {
        library_ = std::make_unique<LibraryWindow>(*libraryClient_);
        connect(library_.get(), &LibraryWindow::settingsRequested, this, &ScreenFrameApplication::showSettings,
                Qt::QueuedConnection);
    }
    bringToFront(*library_);
}

void ScreenFrameApplication::showSettings()
{
    if (busy_ || quitting_ || settingsWindow_)
        return;

    QString settingsError;
    const AppSettings saved = loadSettings(defaultSettingsDirectory(), &settingsError);
    QWidget *parent = library_ && library_->isVisible() ? static_cast<QWidget *>(library_.get()) : editor_.get();
    settingsWindow_ = std::make_unique<SettingsWindow>(saved, parent);
    connect(settingsWindow_.get(), &SettingsWindow::testRequested, this, [this](const LibrarySettings &settings) {
        testClient_ = std::make_unique<LibraryClient>(settings);
        testClient_->list({}, settingsWindow_.get(), [this](QList<LibraryItem>, const QString &, const QString &error) {
            settingsWindow_->setTestResult(error);
        });
    });
    if (!settingsError.isEmpty()) {
        QMessageBox::warning(parent, QStringLiteral("ScreenFrame settings"),
                             settingsError + QStringLiteral("\n\nSave new settings to replace the unreadable configuration."));
    }
    const bool accepted = settingsWindow_->exec() == QDialog::Accepted;
    const AppSettings updated = settingsWindow_->settings();
    settingsWindow_.reset();
    testClient_.reset();
    if (!accepted)
        return;

    const bool libraryChanged = updated.library != settings_.library;
    const bool destinationChanged = updated.library.storageKey() != settings_.library.storageKey();
    if (libraryChanged && !flushLibrary()) {
        QMessageBox::warning(parent, QStringLiteral("Settings could not be changed"),
                             QStringLiteral("ScreenFrame could not save its pending screenshots locally. Try again after the queue error clears."));
        return;
    }
    QString error;
    if ((updated != saved || !settingsError.isEmpty()) && !saveSettings(updated, defaultSettingsDirectory(), &error)) {
        QMessageBox::warning(parent, QStringLiteral("Settings could not be saved"), error);
        return;
    }

    const bool shortcutChanged = updated.useSystemRegionShortcut != settings_.useSystemRegionShortcut;
    const bool libraryWasVisible = library_ && library_->isVisible();
    const bool keepPreviewLocal = previewCapture_.capturedAt.isValid()
        && (destinationChanged || updated.library.autoArchive != settings_.library.autoArchive);
    if (keepPreviewLocal)
        previewCapture_ = {};
    settings_ = updated;
    const bool hadSettingsError = !settingsError_.isEmpty();
    settingsError_.clear();
    if (libraryChanged) {
        library_.reset();
        libraryClient_ = std::make_unique<LibraryClient>(settings_.library);
        connectLibraryClient();
        refreshLibraryStatus(0, {});
        libraryClient_->start();
        if (libraryWasVisible)
            showLibrary();
    } else if (hadSettingsError) {
        refreshLibraryStatus(0, {});
    }
    if (shortcutChanged || keepPreviewLocal) {
        QString message;
        if (shortcutChanged)
            message = QStringLiteral("Quit and reopen ScreenFrame to apply the shortcut change.");
        if (keepPreviewLocal) {
            if (!message.isEmpty())
                message += QStringLiteral("\n\n");
            message += QStringLiteral("The screenshot already open will stay local. Use Save PNG to keep a copy.");
        }
        QWidget *messageParent = libraryWasVisible ? static_cast<QWidget *>(library_.get()) : editor_.get();
        QMessageBox::information(messageParent, QStringLiteral("Settings saved"), message);
    }
}

void ScreenFrameApplication::quitDesktop()
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

void ScreenFrameApplication::beginCapture(bool region, bool fromTrayMenu)
{
    if (busy_ || quitting_ || settingsWindow_) {
        return;
    }

    busy_ = true;
    region_ = region;
    portalAuthorizationAttempted_ = false;
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

void ScreenFrameApplication::captureAfterHiding(bool surfaceWasVisible)
{
    const auto capture = [this] { capture_.capture(region_); };
#ifdef Q_OS_MACOS
    // ScreenCaptureKit filters out ScreenFrame even while its windows are being hidden.
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

void ScreenFrameApplication::finishCapture(QImage image)
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

    const QString kind = region_ ? QStringLiteral("region") : QStringLiteral("full");
    const QDateTime capturedAt = QDateTime::currentDateTimeUtc();
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

    if (!options_.preview) {
        libraryClient_->enqueue(std::move(image), kind, capturedAt);
        exitAfterLibrarySync();
        return;
    }

    restoreEditor_ = false;
    restoreLibrary_ = false;
    archivePreview();
    image.setDevicePixelRatio(1);
    editor().setCapture(image);
    previewCapture_ = {kind, capturedAt};
    showEditor();
    previewOpened_ = true;
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

void ScreenFrameApplication::failCapture(const QString &message)
{
    busy_ = false;
    portalAuthorizing_ = false;
    const bool shouldQuit = std::exchange(exitAfterCapture_, false);
    if (!isDesktop() && !previewOpened_) {
        QTextStream(stderr) << message << Qt::endl;
        if (options_.preview) {
            QMessageBox::warning(nullptr, QStringLiteral("ScreenFrame"), message);
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

void ScreenFrameApplication::requestPortalPermission(const QString &message)
{
    if (!options_.preview || std::exchange(portalAuthorizationAttempted_, true)) {
        failCapture(message);
        return;
    }

    auto &window = editor();
    bringToFront(window);
    QMessageBox dialog(QMessageBox::Question, QStringLiteral("Screen Capture Permission"), message,
                       QMessageBox::NoButton, &window);
    dialog.setInformativeText(QStringLiteral("ScreenFrame will request access with this window open. "
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
}

void ScreenFrameApplication::restoreWindows()
{
    if (std::exchange(restoreEditor_, false)) {
        showEditor();
    }
    if (std::exchange(restoreLibrary_, false)) {
        showLibrary();
    }
}

void ScreenFrameApplication::archivePreview()
{
    if (!previewCapture_.capturedAt.isValid()) {
        return;
    }
    const auto capture = std::exchange(previewCapture_, {});
    if (editor_) {
        libraryClient_->enqueue(editor_->currentImage(), capture.kind, capture.capturedAt);
    }
}

bool ScreenFrameApplication::flushLibrary()
{
    if (libraryClient_->flushPendingWrites()) {
        libraryFlushFailed_ = false;
        return true;
    }
    const QString error = QStringLiteral("Could not save a screenshot to the local Library queue.");
    if (!isDesktop() && !libraryFlushFailed_) {
        QTextStream(stderr) << error << Qt::endl;
    }
    libraryFlushFailed_ = true;
    refreshLibraryStatus(libraryClient_->pendingCount(), error);
    return false;
}

void ScreenFrameApplication::exitAfterLibrarySync()
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

void ScreenFrameApplication::refreshLibraryStatus(int pending, const QString &error)
{
    if (error.isEmpty()) {
        libraryMessage_.clear();
    } else if (pending > 0) {
        libraryMessage_ = QStringLiteral("Library (%1 pending): %2").arg(pending).arg(error);
    } else {
        libraryMessage_ = QStringLiteral("Library: %1").arg(error);
    }
    tray_.setToolTip(libraryMessage_.isEmpty() ? QStringLiteral("ScreenFrame") : libraryMessage_);
    if (editor_) {
        editor_->setLibraryStatus(libraryMessage_);
    }
}

void ScreenFrameApplication::refreshShortcuts(bool full, bool region, const QString &message)
{
    fullShortcut_ = full ? shortcuts_.description(false) : QString{};
    regionShortcut_ = region ? shortcuts_.description(true) : QString{};
    shortcutMessage_ = message;
    if (editor_) {
        editor_->setShortcuts(fullShortcut_, regionShortcut_, shortcutMessage_);
    }
}
