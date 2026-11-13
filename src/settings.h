#pragma once

#include <QString>

#include <optional>

enum class LibraryTransport { Local, Ssh, Tailscale };

constexpr int defaultLibraryPort = 18745;
constexpr int defaultHostedLibraryPort = 18746;

struct LibrarySettings {
    LibraryTransport transport = LibraryTransport::Local;
    QString sshHost;
    int port = defaultLibraryPort;
    QString token;
    QString device;
    bool autoArchive = false;
    QString address;

    bool isConfigured() const;
    QString validationError() const;
    QString storageKey() const;
    bool operator==(const LibrarySettings &) const = default;
};

struct HostedLibrarySettings {
    bool enabled = false;
    QString directory;
    QString token;
    int port = defaultHostedLibraryPort;
    bool share = false;

    QString validationError() const;
    bool operator==(const HostedLibrarySettings &) const = default;
};

struct AppSettings {
    LibrarySettings library;
    bool useSystemRegionShortcut = false;
    HostedLibrarySettings hostedLibrary;

    bool operator==(const AppSettings &) const = default;
};

QString defaultSettingsDirectory();
QString defaultHostedLibraryDirectory(const QString &settingsDirectory = defaultSettingsDirectory());
QString defaultLibraryDeviceName();
QString generateLibraryToken();
bool isTailscaleAddress(const QString &address);
QString libraryConnectionCode(const LibrarySettings &settings);
std::optional<LibrarySettings> librarySettingsFromConnectionCode(const QString &code, const QString &device,
                                                               QString *error = nullptr);
AppSettings loadSettings(const QString &directory, QString *error = nullptr);
bool saveSettings(const AppSettings &settings, const QString &directory, QString *error = nullptr);
QString libraryQueueDirectory(const LibrarySettings &settings, const QString &directory);
