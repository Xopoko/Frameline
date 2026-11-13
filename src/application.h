#pragma once

#include "capture.h"
#include "editor.h"
#include "settings.h"
#include "shortcuts.h"

#include <QDateTime>
#include <QMenu>
#include <QSystemTrayIcon>

#include <memory>

class LibraryClient;
class LibraryWindow;
class SettingsWindow;

struct LaunchOptions {
    enum class Mode { Desktop, Library, Full, Region };

    Mode mode = Mode::Desktop;
    QString output;
    bool trayOnly = false;
    bool preview = true;
};

class ScreenFrameApplication final : public QObject {
public:
    explicit ScreenFrameApplication(LaunchOptions options);
    ~ScreenFrameApplication() override;
    void start();

private:
    EditorWindow &editor();
    void showEditor();
    void showLibrary();
    void showSettings();
    void connectLibraryClient();
    void quitDesktop();
    void beginCapture(bool region, bool fromTrayMenu = false);
    void captureAfterHiding(bool surfaceWasVisible);
    void finishCapture(QImage image);
    void failCapture(const QString &message);
    void requestPortalPermission(const QString &message);
    void restoreWindows();
    void refreshShortcuts(bool full, bool region, const QString &message);
    void refreshLibraryStatus(int pending, const QString &error);
    void archivePreview();
    bool flushLibrary();
    void exitAfterLibrarySync();
    bool isDesktop() const;

    struct PreviewCapture {
        QString kind;
        QDateTime capturedAt;
    };

    LaunchOptions m_options;
    AppSettings m_settings;
    QString m_settingsError;
    ScreenCapture m_capture;
    GlobalShortcuts m_shortcuts;
    std::unique_ptr<LibraryClient> m_libraryClient;
    std::unique_ptr<EditorWindow> m_editor;
    std::unique_ptr<LibraryWindow> m_library;
    std::unique_ptr<SettingsWindow> m_settingsWindow;
    std::unique_ptr<LibraryClient> m_testClient;
    std::unique_ptr<QMenu> m_trayMenu;
    QSystemTrayIcon m_tray;
    QString m_fullShortcut;
    QString m_regionShortcut;
    QString m_shortcutMessage;
    QString m_libraryMessage;
    PreviewCapture m_previewCapture;
    int m_exitCode = 0;
    bool m_busy = false;
    bool m_region = false;
    bool m_restoreEditor = false;
    bool m_restoreLibrary = false;
    bool m_previewOpened = false;
    bool m_quitting = false;
    bool m_exitAfterCapture = false;
    bool m_libraryFlushFailed = false;
    bool m_portalAuthorizationAttempted = false;
    bool m_portalAuthorizing = false;
};
