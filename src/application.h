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

    LaunchOptions options_;
    AppSettings settings_;
    QString settingsError_;
    ScreenCapture capture_;
    GlobalShortcuts shortcuts_;
    std::unique_ptr<LibraryClient> libraryClient_;
    std::unique_ptr<EditorWindow> editor_;
    std::unique_ptr<LibraryWindow> library_;
    std::unique_ptr<SettingsWindow> settingsWindow_;
    std::unique_ptr<LibraryClient> testClient_;
    std::unique_ptr<QMenu> trayMenu_;
    QSystemTrayIcon tray_;
    QString fullShortcut_;
    QString regionShortcut_;
    QString shortcutMessage_;
    QString libraryMessage_;
    PreviewCapture previewCapture_;
    int exitCode_ = 0;
    bool busy_ = false;
    bool region_ = false;
    bool restoreEditor_ = false;
    bool restoreLibrary_ = false;
    bool previewOpened_ = false;
    bool quitting_ = false;
    bool exitAfterCapture_ = false;
    bool libraryFlushFailed_ = false;
    bool portalAuthorizationAttempted_ = false;
    bool portalAuthorizing_ = false;
};
