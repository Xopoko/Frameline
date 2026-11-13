#pragma once

#include "capture.h"
#include "editor.h"
#include "shortcuts.h"

#include <QMenu>
#include <QSystemTrayIcon>

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
    void showEditor();
    void beginCapture(bool region);
    void finishCapture(QImage image);
    void failCapture(const QString &message);
    void requestPortalPermission(const QString &message);
    void restoreEditor();
    void refreshShortcuts(bool full, bool region, const QString &message);

    LaunchOptions m_options;
    ScreenCapture m_capture;
    GlobalShortcuts m_shortcuts;
    EditorWindow m_editor;
    QMenu m_trayMenu;
    QSystemTrayIcon m_tray;
    bool m_busy = false;
    bool m_region = false;
    bool m_restoreEditor = false;
    bool m_previewOpened = false;
    bool m_portalAuthorizationAttempted = false;
    bool m_portalAuthorizing = false;
};
