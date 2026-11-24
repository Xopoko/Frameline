#pragma once

#include "capture.h"
#include "editor.h"
#include "settings.h"
#include "shortcuts.h"

#include <QDateTime>
#include <QMenu>
#include <QSystemTrayIcon>

#include <functional>
#include <memory>

class LibraryClient;
class LibraryWindow;
class SettingsWindow;
class LibraryServer;
class TailscaleMonitor;

struct LaunchOptions {
    enum class Mode { Desktop, Library, Full, Region, Settings };

    Mode mode = Mode::Desktop;
    QString output;
    bool trayOnly = false;
    bool preview = true;
};

class FramelineApplication final : public QObject {
public:
    explicit FramelineApplication(LaunchOptions options);
    ~FramelineApplication() override;
    void start();
    void activate(LaunchOptions::Mode mode);

private:
    EditorWindow &editor();
    void showEditor();
    void showLibrary();
    void showSettings();
    void saveWindowSettings(AppSettings updated, bool autostart);
    bool startHostedLibrary(const HostedLibrarySettings &settings, QString *error);
    void refreshHostedSharing();
    void connectLibraryClient();
    void quitDesktop();
    void beginCapture(bool region, bool fromTrayMenu = false);
    void captureAfterHiding(bool surfaceWasVisible);
    void finishCapture(QImage image, const std::function<void()> &dismissSelection = {});
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
        QString id;
    };

    LaunchOptions options_;
    AppSettings settings_;
    QString settingsError_;
    ScreenCapture capture_;
    GlobalShortcuts shortcuts_;
    std::unique_ptr<LibraryClient> history_;
    std::unique_ptr<LibraryClient> libraryClient_;
    std::unique_ptr<EditorWindow> editor_;
    std::unique_ptr<LibraryWindow> library_;
    std::unique_ptr<SettingsWindow> settingsWindow_;
    std::unique_ptr<LibraryClient> testClient_;
    std::unique_ptr<LibraryServer> libraryServer_;
    std::unique_ptr<TailscaleMonitor> tailscale_;
    std::unique_ptr<QMenu> trayMenu_;
    QSystemTrayIcon tray_;
    QString fullShortcut_;
    QString regionShortcut_;
    QString shortcutMessage_;
    QString libraryMessage_;
    QString historyError_;
    QString syncError_;
    QString hostedLibraryError_;
    QString hostedSharingError_;
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
    bool restoreLibraryAfterSettings_ = false;
};
