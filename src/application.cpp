#include "application.h"

#include "libraryclient.h"
#include "librarywindow.h"
#include "pngexport.h"
#include "regionselector.h"

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

ScreenFrameApplication::ScreenFrameApplication(LaunchOptions options)
    : m_options(std::move(options)), m_libraryClient(std::make_unique<LibraryClient>())
{
    connect(m_libraryClient.get(), &LibraryClient::syncStatusChanged, this, &ScreenFrameApplication::refreshLibraryStatus);
    connect(qApp, &QApplication::aboutToQuit, this, [this] {
        archivePreview();
        flushLibrary();
    });
    connect(qApp, &QApplication::lastWindowClosed, this, [this] {
        if (!m_tray.isVisible() && !m_busy && (isDesktop() || m_previewOpened)) {
            exitAfterLibrarySync();
        }
    });
    connect(&m_shortcuts, &GlobalShortcuts::captureRequested, this, [this](bool region) { beginCapture(region); });
    connect(&m_shortcuts, &GlobalShortcuts::statusChanged, this, &ScreenFrameApplication::refreshShortcuts);
    connect(&m_capture, &ScreenCapture::failed, this, &ScreenFrameApplication::failCapture);
    connect(&m_capture, &ScreenCapture::portalPermissionRequired, this, &ScreenFrameApplication::requestPortalPermission);
    connect(&m_capture, &ScreenCapture::cancelled, this, [this] { finishCapture({}); });
    connect(&m_capture, &ScreenCapture::captured, this, [this](const QList<CaptureFrame> &frames) {
        if (std::exchange(m_portalAuthorizing, false)) {
            m_editor->hide();
            captureAfterHiding(true);
            return;
        }
        if (!m_region || frames.isEmpty()) {
            finishCapture(frames.isEmpty() ? QImage{} : frames.first().image);
            return;
        }
        RegionSelector::start(frames, [this](QImage image) { finishCapture(std::move(image)); });
    });
}

ScreenFrameApplication::~ScreenFrameApplication() = default;

bool ScreenFrameApplication::isDesktop() const
{
    return m_options.mode == LaunchOptions::Mode::Desktop || m_options.mode == LaunchOptions::Mode::Library;
}

void ScreenFrameApplication::start()
{
    // The capture overlay can briefly be the last visible window. Closing the final editor
    // should also give the local Library queue time to finish writing.
    QApplication::setQuitOnLastWindowClosed(false);
    m_libraryClient->start();
    if (!isDesktop()) {
        QTimer::singleShot(0, this, [this] { beginCapture(m_options.mode == LaunchOptions::Mode::Region); });
        return;
    }

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_tray.setIcon(QApplication::windowIcon());
        m_tray.setToolTip(m_libraryMessage.isEmpty() ? QStringLiteral("ScreenFrame") : m_libraryMessage);
        m_trayMenu = std::make_unique<QMenu>();
        m_trayMenu->addAction(QStringLiteral("Open ScreenFrame"), this, &ScreenFrameApplication::showEditor);
        m_trayMenu->addAction(QStringLiteral("Library"), this, &ScreenFrameApplication::showLibrary);
        m_trayMenu->addSeparator();
        m_trayMenu->addAction(QStringLiteral("Capture Screen"), this, [this] { beginCapture(false, true); });
        m_trayMenu->addAction(QStringLiteral("Capture Region"), this, [this] { beginCapture(true, true); });
        m_trayMenu->addSeparator();
        m_trayMenu->addAction(QStringLiteral("Quit"), this, &ScreenFrameApplication::quitDesktop);
        m_tray.setContextMenu(m_trayMenu.get());
        connect(&m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger) {
                showEditor();
            }
        });
        m_tray.show();
    }

    m_shortcuts.start();
    if (m_options.mode == LaunchOptions::Mode::Library) {
        showLibrary();
    } else if (!m_options.trayOnly || !m_tray.isVisible()) {
        showEditor();
    }
}

EditorWindow &ScreenFrameApplication::editor()
{
    if (!m_editor) {
        m_editor = std::make_unique<EditorWindow>();
        m_editor->setShortcuts(m_fullShortcut, m_regionShortcut, m_shortcutMessage);
        m_editor->setLibraryStatus(m_libraryMessage);
        connect(m_editor.get(), &EditorWindow::fullCaptureRequested, this, [this] { beginCapture(false); });
        connect(m_editor.get(), &EditorWindow::regionCaptureRequested, this, [this] { beginCapture(true); });
        connect(m_editor.get(), &EditorWindow::libraryRequested, this, &ScreenFrameApplication::showLibrary);
        connect(m_editor.get(), &EditorWindow::closeAccepted, this, &ScreenFrameApplication::archivePreview);
    }
    return *m_editor;
}

void ScreenFrameApplication::showEditor()
{
    if (m_busy || m_quitting) {
        return;
    }
    auto &window = editor();
    window.showNormal();
    window.raise();
    window.activateWindow();
}

void ScreenFrameApplication::showLibrary()
{
    if (m_busy || m_quitting) {
        return;
    }
    if (!m_library) {
        m_library = std::make_unique<LibraryWindow>(*m_libraryClient);
    }
    m_library->showNormal();
    m_library->raise();
    m_library->activateWindow();
}

void ScreenFrameApplication::quitDesktop()
{
    if (m_quitting) {
        return;
    }
    if (m_busy) {
        m_exitAfterCapture = true;
        return;
    }
    if (m_editor && m_editor->hasUnsavedChanges()) {
        showEditor();
        if (!m_editor->confirmReplace()) {
            return;
        }
    }
    archivePreview();
    exitAfterLibrarySync();
}

void ScreenFrameApplication::beginCapture(bool region, bool fromTrayMenu)
{
    if (m_busy || m_quitting) {
        return;
    }

    m_busy = true;
    m_region = region;
    m_portalAuthorizationAttempted = false;
    m_restoreEditor = m_editor && m_editor->isVisible();
    m_restoreLibrary = m_library && m_library->isVisible() && !m_library->isMinimized();
    if (m_editor && m_editor->hasUnsavedChanges()) {
        m_editor->showNormal();
        m_editor->raise();
        m_editor->activateWindow();
        if (!m_editor->confirmReplace()) {
            m_busy = false;
            if (std::exchange(m_exitAfterCapture, false)) {
                quitDesktop();
            }
            return;
        }
    }

    const bool surfaceWasVisible = fromTrayMenu || (m_trayMenu && m_trayMenu->isVisible())
        || m_restoreLibrary || (m_editor && m_editor->isVisible() && !m_editor->isMinimized());
    if (m_editor) {
        m_editor->hide();
    }
    if (m_trayMenu) {
        m_trayMenu->hide();
    }
    if (m_library) {
        m_library->hide();
    }
    captureAfterHiding(surfaceWasVisible);
}

void ScreenFrameApplication::captureAfterHiding(bool surfaceWasVisible)
{
    const auto capture = [this] { m_capture.capture(m_region); };
#ifdef Q_OS_MACOS
    // ScreenCaptureKit filters out ScreenFrame even while its windows are being hidden.
    Q_UNUSED(surfaceWasVisible);
    QTimer::singleShot(0, this, capture);
#else
    constexpr int compositorDelayMs = 180;
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
    m_busy = false;
    m_portalAuthorizing = false;
    const bool shouldQuit = std::exchange(m_exitAfterCapture, false);
    if (image.isNull()) {
        if (isDesktop() || m_previewOpened) {
            restoreWindows();
        } else {
            QApplication::exit(2);
        }
        if (shouldQuit) {
            quitDesktop();
        }
        return;
    }

    const QString kind = m_region ? QStringLiteral("region") : QStringLiteral("full");
    const QDateTime capturedAt = QDateTime::currentDateTimeUtc();
    QString outputError;
    if (!m_options.output.isEmpty()) {
        QString error;
        if (!savePng(image, m_options.output, &error)) {
            outputError = QStringLiteral("Could not save the PNG to %1. %2").arg(m_options.output, error);
            QTextStream(stderr) << outputError << Qt::endl;
            m_exitCode = 1;
        }
        m_options.output.clear();
    }

    if (!m_options.preview) {
        m_libraryClient->enqueue(std::move(image), kind, capturedAt);
        exitAfterLibrarySync();
        return;
    }

    m_restoreEditor = false;
    m_restoreLibrary = false;
    archivePreview();
    image.setDevicePixelRatio(1);
    editor().setCapture(image);
    m_previewCapture = {kind, capturedAt};
    showEditor();
    m_previewOpened = true;
    if (!outputError.isEmpty()) {
        QMessageBox dialog(QMessageBox::Warning, QStringLiteral("Could not save screenshot"), outputError,
                           QMessageBox::Ok, m_editor.get());
        dialog.setTextFormat(Qt::PlainText);
        dialog.exec();
    }
    if (shouldQuit) {
        quitDesktop();
    }
}

void ScreenFrameApplication::failCapture(const QString &message)
{
    m_busy = false;
    m_portalAuthorizing = false;
    const bool shouldQuit = std::exchange(m_exitAfterCapture, false);
    if (!isDesktop() && !m_previewOpened) {
        QTextStream(stderr) << message << Qt::endl;
        if (m_options.preview) {
            QMessageBox::warning(nullptr, QStringLiteral("ScreenFrame"), message);
        }
        m_exitCode = 1;
        exitAfterLibrarySync();
        return;
    }

    restoreWindows();
    showEditor();
    QMessageBox::warning(m_editor.get(), QStringLiteral("Capture Failed"), message);
    if (shouldQuit) {
        quitDesktop();
    }
}

void ScreenFrameApplication::requestPortalPermission(const QString &message)
{
    if (!m_options.preview || std::exchange(m_portalAuthorizationAttempted, true)) {
        failCapture(message);
        return;
    }

    auto &window = editor();
    window.showNormal();
    window.raise();
    window.activateWindow();
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

    m_portalAuthorizing = true;
    QTimer::singleShot(180, this, [this] { m_capture.capture(m_region, m_editor.get()); });
}

void ScreenFrameApplication::restoreWindows()
{
    if (std::exchange(m_restoreEditor, false)) {
        showEditor();
    }
    if (std::exchange(m_restoreLibrary, false)) {
        showLibrary();
    }
}

void ScreenFrameApplication::archivePreview()
{
    if (!m_previewCapture.capturedAt.isValid()) {
        return;
    }
    const auto capture = std::exchange(m_previewCapture, {});
    if (m_editor) {
        m_libraryClient->enqueue(m_editor->currentImage(), capture.kind, capture.capturedAt);
    }
}

bool ScreenFrameApplication::flushLibrary()
{
    if (m_libraryClient->flushPendingWrites()) {
        m_libraryFlushFailed = false;
        return true;
    }
    const QString error = QStringLiteral("Could not save a screenshot to the local Library queue.");
    if (!isDesktop() && !m_libraryFlushFailed) {
        QTextStream(stderr) << error << Qt::endl;
    }
    m_libraryFlushFailed = true;
    refreshLibraryStatus(m_libraryClient->pendingCount(), error);
    return false;
}

void ScreenFrameApplication::exitAfterLibrarySync()
{
    if (m_quitting) {
        return;
    }
    if (!flushLibrary()) {
        if (isDesktop() || m_previewOpened) {
            showEditor();
            return;
        }
        m_exitCode = 1;
    }
    m_quitting = true;

    const auto exit = [this] { QApplication::exit(m_exitCode); };
    if (m_libraryClient->pendingCount() == 0) {
        QTimer::singleShot(0, this, exit);
        return;
    }

    connect(m_libraryClient.get(), &LibraryClient::syncStatusChanged, this,
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
        m_libraryMessage.clear();
    } else if (pending > 0) {
        m_libraryMessage = QStringLiteral("Library (%1 pending): %2").arg(pending).arg(error);
    } else {
        m_libraryMessage = QStringLiteral("Library: %1").arg(error);
    }
    m_tray.setToolTip(m_libraryMessage.isEmpty() ? QStringLiteral("ScreenFrame") : m_libraryMessage);
    if (m_editor) {
        m_editor->setLibraryStatus(m_libraryMessage);
    }
}

void ScreenFrameApplication::refreshShortcuts(bool full, bool region, const QString &message)
{
    m_fullShortcut = full ? m_shortcuts.description(false) : QString{};
    m_regionShortcut = region ? m_shortcuts.description(true) : QString{};
    m_shortcutMessage = message;
    if (m_editor) {
        m_editor->setShortcuts(m_fullShortcut, m_regionShortcut, m_shortcutMessage);
    }
}
