#include "settings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <optional>

namespace {

void report(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

std::optional<QJsonObject> readObject(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8192) {
        report(error, QObject::tr("Could not read ScreenFrame settings."));
        return {};
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        report(error, QObject::tr("ScreenFrame settings are not valid JSON."));
        return {};
    }
    return document.object();
}

LibrarySettings readLibrary(const QJsonObject &object, bool legacy, QString *error)
{
    LibrarySettings library;
    library.sshHost = object.value(QStringLiteral("sshHost")).toString();
    const QString transport = object.value(QStringLiteral("transport")).toString();
    if (legacy) {
        library.transport = library.sshHost.isEmpty() ? LibraryTransport::Local : LibraryTransport::Ssh;
    } else if (transport == QStringLiteral("ssh")) {
        library.transport = LibraryTransport::Ssh;
    } else if (transport != QStringLiteral("local")) {
        report(error, QObject::tr("Unknown library connection type."));
        return {};
    }
    const auto port = object.value(QStringLiteral("port"));
    library.port = port.isUndefined() ? 18745 : port.toInt(0);
    library.token = object.value(QStringLiteral("token")).toString();
    library.device = object.value(QStringLiteral("device")).toString();
    library.autoArchive = legacy || object.value(QStringLiteral("autoArchive")).toBool();
    const QString validation = library.validationError();
    if (!validation.isEmpty()) {
        report(error, validation);
        return {};
    }
    return library;
}

LibrarySettings readLegacy(const QDir &directory, QString *error = nullptr)
{
    const QString path = directory.filePath(QStringLiteral("library.json"));
    if (!QFileInfo::exists(path))
        return {};
    const auto object = readObject(path, error);
    return object ? readLibrary(*object, true, error) : LibrarySettings{};
}

QString transportName(LibraryTransport transport)
{
    return transport == LibraryTransport::Ssh ? QStringLiteral("ssh") : QStringLiteral("local");
}
} // namespace

QString LibrarySettings::validationError() const
{
    if (port < 1 || port > 65535)
        return QObject::tr("Port must be between 1 and 65535.");
    if (transport == LibraryTransport::Local && sshHost.isEmpty() && token.isEmpty()
        && device.isEmpty() && !autoArchive && port == 18745)
        return {};

    static const QRegularExpression safeHost(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\\z"));
    static const QRegularExpression safeToken(QStringLiteral("\\A[A-Za-z0-9_-]{32,256}\\z"));
    static const QRegularExpression safeDevice(QStringLiteral("\\A[ -~]{1,48}\\z"));
    if (transport == LibraryTransport::Ssh && !safeHost.match(sshHost).hasMatch())
        return QObject::tr("Enter an SSH host or alias using letters, numbers, dots, underscores, or hyphens.");
    if (transport == LibraryTransport::Local && !sshHost.isEmpty())
        return QObject::tr("A local connection does not use an SSH host.");
    if (!safeToken.match(token).hasMatch())
        return QObject::tr("Enter the library access token (32–256 letters, numbers, underscores, or hyphens).");
    if (!safeDevice.match(device).hasMatch() || device.trimmed().isEmpty())
        return QObject::tr("Enter a device label of 1–48 printable ASCII characters.");
    return {};
}

bool LibrarySettings::isConfigured() const
{
    return !token.isEmpty() && validationError().isEmpty();
}

QString LibrarySettings::storageKey() const
{
    if (!isConfigured())
        return {};
    const QByteArray destination = transportName(transport).toLatin1() + '\n' + sshHost.toLatin1() + '\n'
        + QByteArray::number(port) + '\n' + token.toLatin1();
    return QString::fromLatin1(QCryptographicHash::hash(destination, QCryptographicHash::Sha256).toHex());
}

QString defaultSettingsDirectory()
{
    return QDir::home().filePath(QStringLiteral(".screenframe"));
}

AppSettings loadSettings(const QString &directory, QString *error)
{
    report(error, {});
    const QDir dir(directory);
    const QString path = dir.filePath(QStringLiteral("settings.json"));
    if (!QFileInfo::exists(path)) {
        const auto legacy = readLegacy(dir, error);
        return {legacy, legacy.isConfigured()};
    }
    const auto object = readObject(path, error);
    if (!object)
        return {};
    if (object->value(QStringLiteral("schemaVersion")).toInt() != 1
        || !object->value(QStringLiteral("library")).isObject()) {
        report(error, QObject::tr("Unsupported ScreenFrame settings format."));
        return {};
    }
    return {readLibrary(object->value(QStringLiteral("library")).toObject(), false, error),
            object->value(QStringLiteral("useSystemRegionShortcut")).toBool()};
}

bool saveSettings(const AppSettings &settings, const QString &directory, QString *error)
{
    const auto &library = settings.library;
    const QString validation = library.validationError();
    report(error, validation);
    if (!validation.isEmpty())
        return false;
    if (!QDir().mkpath(directory)) {
        report(error, QObject::tr("Could not create the ScreenFrame settings folder."));
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        report(error, QObject::tr("Could not protect the ScreenFrame settings folder."));
        return false;
    }
#endif
    const QJsonObject libraryObject{{QStringLiteral("transport"), transportName(library.transport)},
                                    {QStringLiteral("sshHost"), library.sshHost},
                                    {QStringLiteral("port"), library.port},
                                    {QStringLiteral("token"), library.token},
                                    {QStringLiteral("device"), library.device},
                                    {QStringLiteral("autoArchive"), library.autoArchive}};
    const QJsonObject object{{QStringLiteral("schemaVersion"), 1},
                             {QStringLiteral("library"), libraryObject},
                             {QStringLiteral("useSystemRegionShortcut"), settings.useSystemRegionShortcut}};
    const QByteArray bytes = QJsonDocument(object).toJson();
    QSaveFile file(QDir(directory).filePath(QStringLiteral("settings.json")));
    if (!file.open(QIODevice::WriteOnly)
#ifndef Q_OS_WIN
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
#endif
        || file.write(bytes) != bytes.size() || !file.commit()) {
        report(error, QObject::tr("Could not save ScreenFrame settings. %1").arg(file.errorString()));
        return false;
    }
    return true;
}

QString libraryQueueDirectory(const LibrarySettings &settings, const QString &directory)
{
    const QString key = settings.storageKey();
    if (key.isEmpty())
        return {};
    const QDir dir(directory);
    // Older clients keep one queue and still read library.json. Only its original destination may use that queue.
    if (readLegacy(dir).storageKey() == key)
        return dir.filePath(QStringLiteral("queue"));
    return dir.filePath(QStringLiteral("queues/") + key);
}
