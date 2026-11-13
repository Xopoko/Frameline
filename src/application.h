#pragma once

#include "capture.h"
#include "editor.h"
#include "shortcuts.h"

#include <QMenu>
#include <QSystemTrayIcon>

#include <memory>

struct LaunchOptions {
    enum class Mode { Desktop, Full, Region };

    Mode mode = Mode::Desktop;
    QString output;
    bool trayOnly = false;
    bool preview = true;
};

class ScreenFrameApplication final : public QObject {
public:
    explicit ScreenFrameApplication(LaunchOptions options);
    void start();

private:
    EditorWindow &editor();
    void showEditor();
    void beginCapture(bool region, bool fromTrayMenu = false);
    void captureAfterHiding(bool surfaceWasVisible);
    void finishCapture(QImage image);
    void failCapture(const QString &message);
    void requestPortalPermission(const QString &message);
    void restoreEditor();
    void refreshShortcuts(bool full, bool region, const QString &message);

    LaunchOptions m_options;
    ScreenCapture m_capture;
    GlobalShortcuts m_shortcuts;
    std::unique_ptr<EditorWindow> m_editor;
    std::unique_ptr<QMenu> m_trayMenu;
    QSystemTrayIcon m_tray;
    QString m_fullShortcut;
    QString m_regionShortcut;
    QString m_shortcutMessage;
    bool m_busy = false;
    bool m_region = false;
    bool m_restoreEditor = false;
    bool m_previewOpened = false;
    bool m_portalAuthorizationAttempted = false;
    bool m_portalAuthorizing = false;
};
