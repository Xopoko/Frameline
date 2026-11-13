#pragma once

#include <QString>

enum class LibraryTransport { Local, Ssh };

constexpr int defaultLibraryPort = 18745;

struct LibrarySettings {
    LibraryTransport transport = LibraryTransport::Local;
    QString sshHost;
    int port = defaultLibraryPort;
    QString token;
    QString device;
    bool autoArchive = false;

    bool isConfigured() const;
    QString validationError() const;
    QString storageKey() const;
    bool operator==(const LibrarySettings &) const = default;
};

struct AppSettings {
    LibrarySettings library;
    bool useSystemRegionShortcut = false;

    bool operator==(const AppSettings &) const = default;
};

QString defaultSettingsDirectory();
AppSettings loadSettings(const QString &directory, QString *error = nullptr);
bool saveSettings(const AppSettings &settings, const QString &directory, QString *error = nullptr);
QString libraryQueueDirectory(const LibrarySettings &settings, const QString &directory);
