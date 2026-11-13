#include "application.h"

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
    : m_options(std::move(options))
{
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

void ScreenFrameApplication::start()
{
    if (m_options.mode != LaunchOptions::Mode::Desktop) {
        // The selection overlay can be the last visible window while a capture is in flight.
        QApplication::setQuitOnLastWindowClosed(false);
        QTimer::singleShot(0, this, [this] { beginCapture(m_options.mode == LaunchOptions::Mode::Region); });
        return;
    }

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        QApplication::setQuitOnLastWindowClosed(false);
        m_tray.setIcon(QApplication::windowIcon());
        m_tray.setToolTip(QStringLiteral("ScreenFrame"));
        m_trayMenu = std::make_unique<QMenu>();
        m_trayMenu->addAction(QStringLiteral("Открыть ScreenFrame"), this, &ScreenFrameApplication::showEditor);
        m_trayMenu->addSeparator();
        m_trayMenu->addAction(QStringLiteral("Снять экран"), this, [this] { beginCapture(false, true); });
        m_trayMenu->addAction(QStringLiteral("Снять область"), this, [this] { beginCapture(true, true); });
        m_trayMenu->addSeparator();
        m_trayMenu->addAction(QStringLiteral("Выйти"), this, [this] {
            if (m_editor && !m_editor->isVisible() && m_editor->hasUnsavedChanges()) {
                showEditor();
            }
            if (!m_editor || m_editor->confirmReplace()) {
                QApplication::quit();
            }
        });
        m_tray.setContextMenu(m_trayMenu.get());
        connect(&m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger) {
                showEditor();
            }
        });
        m_tray.show();
    }

    m_shortcuts.start();
    if (!m_options.trayOnly || !m_tray.isVisible()) {
        showEditor();
    }
}

EditorWindow &ScreenFrameApplication::editor()
{
    if (!m_editor) {
        m_editor = std::make_unique<EditorWindow>();
        m_editor->setShortcuts(m_fullShortcut, m_regionShortcut, m_shortcutMessage);
        connect(m_editor.get(), &EditorWindow::fullCaptureRequested, this, [this] { beginCapture(false); });
        connect(m_editor.get(), &EditorWindow::regionCaptureRequested, this, [this] { beginCapture(true); });
    }
    return *m_editor;
}

void ScreenFrameApplication::showEditor()
{
    if (m_busy) {
        return;
    }
    auto &window = editor();
    window.showNormal();
    window.raise();
    window.activateWindow();
}

void ScreenFrameApplication::beginCapture(bool region, bool fromTrayMenu)
{
    if (m_busy) {
        return;
    }

    m_busy = true;
    m_region = region;
    m_portalAuthorizationAttempted = false;
    m_restoreEditor = m_editor && m_editor->isVisible();
    if (m_editor && m_editor->hasUnsavedChanges()) {
        m_editor->showNormal();
        m_editor->raise();
        m_editor->activateWindow();
        if (!m_editor->confirmReplace()) {
            m_busy = false;
            return;
        }
    }

    const bool surfaceWasVisible = fromTrayMenu || (m_trayMenu && m_trayMenu->isVisible())
        || (m_editor && m_editor->isVisible() && !m_editor->isMinimized());
    if (m_editor) {
        m_editor->hide();
    }
    if (m_trayMenu) {
        m_trayMenu->hide();
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
    const bool desktop = m_options.mode == LaunchOptions::Mode::Desktop;
    if (image.isNull()) {
        if (desktop || m_previewOpened) {
            restoreEditor();
        } else {
            QApplication::exit(2);
        }
        return;
    }

    if (!m_options.output.isEmpty()) {
        QString error;
        if (!savePng(image, m_options.output, &error)) {
            failCapture(QStringLiteral("Не удалось записать PNG в %1. %2").arg(m_options.output, error));
            return;
        }
        m_options.output.clear();
    }

    if (!m_options.preview) {
        QApplication::quit();
        return;
    }

    m_restoreEditor = false;
    image.setDevicePixelRatio(1);
    editor().setCapture(image);
    showEditor();
    m_previewOpened = true;
    if (!desktop) {
        QApplication::setQuitOnLastWindowClosed(true);
    }
}

void ScreenFrameApplication::failCapture(const QString &message)
{
    m_busy = false;
    m_portalAuthorizing = false;
    if (m_options.mode != LaunchOptions::Mode::Desktop && !m_previewOpened) {
        QTextStream(stderr) << message << Qt::endl;
        if (m_options.preview) {
            QMessageBox::warning(nullptr, QStringLiteral("ScreenFrame"), message);
        }
        QApplication::exit(1);
        return;
    }

    showEditor();
    QMessageBox::warning(m_editor.get(), QStringLiteral("Не удалось сделать скриншот"), message);
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
    QMessageBox dialog(QMessageBox::Question, QStringLiteral("Разрешение на захват экрана"), message,
                       QMessageBox::NoButton, &window);
    dialog.setInformativeText(QStringLiteral("ScreenFrame повторит запрос с открытым окном. "
                                              "После подтверждения окно скроется и будет сделан новый снимок."));
    const auto *retry = dialog.addButton(QStringLiteral("Запросить доступ"), QMessageBox::AcceptRole);
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

void ScreenFrameApplication::restoreEditor()
{
    if (std::exchange(m_restoreEditor, false)) {
        showEditor();
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
