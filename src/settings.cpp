#include "settings.h"

#include "libraryprotocol.h"
#include "tailscale.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>

#include <array>

namespace {

void report(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

bool validPort(int port)
{
    return port >= 1 && port <= 65535;
}

std::optional<QJsonObject> readObject(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8192) {
        report(error, QStringLiteral("Could not read Frameline settings."));
        return {};
    }
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        report(error, QStringLiteral("Frameline settings are not valid JSON."));
        return {};
    }
    return document.object();
}

LibrarySettings readLibrary(const QJsonObject &object, QString *error)
{
    LibrarySettings library;
    library.sshHost = object.value(QStringLiteral("sshHost")).toString();
    library.address = object.value(QStringLiteral("address")).toString();
    const QString transport = object.value(QStringLiteral("transport")).toString();
    if (transport == QStringLiteral("ssh")) {
        library.transport = LibraryTransport::Ssh;
    } else if (transport == QStringLiteral("tailscale")) {
        library.transport = LibraryTransport::Tailscale;
    } else if (transport != QStringLiteral("local")) {
        report(error, QStringLiteral("Unknown library connection type."));
        return {};
    }
    const auto port = object.value(QStringLiteral("port"));
    library.port = port.isUndefined() ? defaultLibraryPort : port.toInt(0);
    library.token = object.value(QStringLiteral("token")).toString();
    library.device = object.value(QStringLiteral("device")).toString();
    library.autoArchive = object.value(QStringLiteral("autoArchive")).toBool();
    const QString validation = library.validationError();
    if (!validation.isEmpty()) {
        report(error, validation);
        return {};
    }
    return library;
}

QString transportName(LibraryTransport transport)
{
    switch (transport) {
    case LibraryTransport::Ssh:
        return QStringLiteral("ssh");
    case LibraryTransport::Tailscale:
        return QStringLiteral("tailscale");
    case LibraryTransport::Local:
        return QStringLiteral("local");
    }
    return {};
}

HostedLibrarySettings readHostedLibrary(const QJsonValue &value, QString *error)
{
    if (value.isUndefined())
        return {};
    if (!value.isObject()) {
        report(error, QStringLiteral("Could not read the library on this computer's settings."));
        return {};
    }

    const auto object = value.toObject();
    const auto enabled = object.value(QStringLiteral("enabled"));
    const auto directory = object.value(QStringLiteral("directory"));
    const auto token = object.value(QStringLiteral("token"));
    const auto share = object.value(QStringLiteral("share"));
    if ((!enabled.isUndefined() && !enabled.isBool()) || (!directory.isUndefined() && !directory.isString())
        || (!token.isUndefined() && !token.isString()) || (!share.isUndefined() && !share.isBool())) {
        report(error, QStringLiteral("Could not read the library on this computer's settings."));
        return {};
    }
    HostedLibrarySettings hosted;
    hosted.enabled = enabled.toBool();
    hosted.directory = directory.toString();
    hosted.token = token.toString();
    const auto port = object.value(QStringLiteral("port"));
    hosted.port = port.isUndefined() ? defaultHostedLibraryPort : port.toInt(0);
    hosted.share = share.toBool();
    const QString validation = hosted.validationError();
    if (!validation.isEmpty()) {
        report(error, validation);
        return {};
    }
    return hosted;
}

QString hostedConnectionError(const AppSettings &settings)
{
    const auto &library = settings.library;
    const auto &hosted = settings.hostedLibrary;
    if (hosted.enabled && library.transport == LibraryTransport::Local && library.token == hosted.token
        && library.port != hosted.port)
        return QStringLiteral("The library on this computer and its connection must use the same port.");
    return {};
}
} // namespace

QString LibrarySettings::validationError() const
{
    if (!validPort(port))
        return QStringLiteral("Port must be between 1 and 65535.");
    if (*this == LibrarySettings{})
        return {};

    static const QRegularExpression safeHost(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\\z"));
    static const QRegularExpression safeDevice(QStringLiteral("\\A[ -~]{1,48}\\z"));
    if (transport == LibraryTransport::Ssh && !safeHost.match(sshHost).hasMatch())
        return QStringLiteral("Enter an SSH host or alias using letters, numbers, dots, underscores, or hyphens.");
    if (transport == LibraryTransport::Local && !sshHost.isEmpty())
        return QStringLiteral("A local connection does not use an SSH host.");
    if (transport == LibraryTransport::Tailscale && (!sshHost.isEmpty() || normalizedTailscaleAddress(address).isEmpty()))
        return QStringLiteral("Paste a Frameline connection code with a valid Tailscale address.");
    if (transport != LibraryTransport::Tailscale && !address.isEmpty())
        return QStringLiteral("A Tailscale address requires a Tailscale connection.");
    if (!LibraryProtocol::isToken(token))
        return QStringLiteral("Enter the library access token (32–256 letters, numbers, underscores, or hyphens).");
    if (!safeDevice.match(device).hasMatch() || device.trimmed().isEmpty())
        return QStringLiteral("Enter a device label of 1–48 printable ASCII characters.");
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
    const QString endpoint = transport == LibraryTransport::Tailscale ? normalizedTailscaleAddress(address) : sshHost;
    const QByteArray destination = transportName(transport).toLatin1() + '\n' + endpoint.toLatin1() + '\n'
        + QByteArray::number(port) + '\n' + token.toLatin1();
    return QString::fromLatin1(QCryptographicHash::hash(destination, QCryptographicHash::Sha256).toHex());
}

QString HostedLibrarySettings::validationError() const
{
    if (!validPort(port))
        return QStringLiteral("Port must be between 1 and 65535.");
    if (!enabled && directory.isEmpty() && token.isEmpty() && !share)
        return {};

    const QString path = QDir::cleanPath(QDir::fromNativeSeparators(directory));
    if (directory.size() > 2048 || directory.contains(QChar::Null) || !QDir::isAbsolutePath(path)
        || path.startsWith(QStringLiteral(":")) || QDir(path).isRoot())
        return QStringLiteral("Choose a folder for the library on this computer.");
    if (!LibraryProtocol::isToken(token))
        return QStringLiteral("The library on this computer needs a valid access token.");
    return {};
}

static QString legacySettingsDirectory()
{
    return QDir::home().filePath(QStringLiteral(".screenframe"));
}

QString defaultSettingsDirectory()
{
    // Frameline was called ScreenFrame. Move its folder once, with settings, queued uploads and a
    // library kept there. If the move fails, for example while an older process holds a file open,
    // keep using the old folder.
    const QString current = QDir::home().filePath(QStringLiteral(".frameline"));
    const QString legacy = legacySettingsDirectory();
    if (!QFileInfo::exists(current) && QFileInfo(legacy).isDir() && !QDir().rename(legacy, current)
        && !QFileInfo::exists(current)) {
        return legacy;
    }
    return current;
}

QString defaultHostedLibraryDirectory(const QString &settingsDirectory)
{
    return QDir(settingsDirectory).absoluteFilePath(QStringLiteral("library"));
}

QString defaultLibraryDeviceName()
{
    QString device = QSysInfo::machineHostName().section(u'.', 0, 0).normalized(QString::NormalizationForm_D);
    static const QRegularExpression nonAscii(QStringLiteral("[^ -~]"));
    device.remove(nonAscii);
    device = device.trimmed().left(48).trimmed();
    return device.isEmpty() ? QStringLiteral("This computer") : device;
}

QString generateLibraryToken()
{
    std::array<quint32, 8> random{};
    QRandomGenerator::system()->fillRange(random.data(), random.size());
    const auto bytes = QByteArray::fromRawData(reinterpret_cast<const char *>(random.data()), sizeof(random));
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString libraryConnectionCode(const LibrarySettings &settings)
{
    if (settings.transport != LibraryTransport::Tailscale || !settings.sshHost.isEmpty()
        || !validPort(settings.port) || !LibraryProtocol::isToken(settings.token))
        return {};
    const QString address = normalizedTailscaleAddress(settings.address);
    if (address.isEmpty())
        return {};
    const QByteArray payload = address.toLatin1() + '\n' + QByteArray::number(settings.port) + '\n'
        + settings.token.toLatin1();
    // "SF1-" stands for the ScreenFrame name; codes already shared keep working.
    return QStringLiteral("SF1-") + QString::fromLatin1(payload.toBase64(QByteArray::Base64UrlEncoding
                                                                        | QByteArray::OmitTrailingEquals));
}

std::optional<LibrarySettings> librarySettingsFromConnectionCode(const QString &code, const QString &device,
                                                               QString *error)
{
    const QString invalid = QStringLiteral("Paste a valid Frameline connection code.");
    report(error, invalid);
    if (code.size() > 512)
        return {};
    const QString trimmed = code.trimmed();
    if (!trimmed.startsWith(QStringLiteral("SF1-")))
        return {};
    const QByteArray encoded = trimmed.mid(4).toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::Base64UrlEncoding
                                                                   | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return {};
    const auto fields = decoded.decoded.split('\n');
    if (fields.size() != 3)
        return {};

    LibrarySettings settings;
    settings.transport = LibraryTransport::Tailscale;
    settings.address = normalizedTailscaleAddress(QString::fromLatin1(fields[0]));
    settings.port = fields[1].toInt();
    settings.token = QString::fromLatin1(fields[2]);
    if (libraryConnectionCode(settings) != trimmed)
        return {};

    settings.device = device.trimmed().isEmpty() ? defaultLibraryDeviceName() : device.trimmed();
    const QString validation = settings.validationError();
    report(error, validation);
    if (!validation.isEmpty())
        return {};
    return settings;
}

// A library folder inside the old ScreenFrame settings folder moved with it to the Frameline one.
static QString movedFromLegacyDirectory(const QString &path)
{
    if (path.isEmpty())
        return path;
    const QString legacy = QDir::cleanPath(legacySettingsDirectory());
    const QString current = QDir::cleanPath(QDir::home().filePath(QStringLiteral(".frameline")));
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    const bool inside = clean == legacy || clean.startsWith(legacy + u'/');
    if (!inside || QFileInfo::exists(legacy) || !QFileInfo(current).isDir())
        return path;
    return QDir::toNativeSeparators(current + clean.mid(legacy.size()));
}

AppSettings loadSettings(const QString &directory, QString *error)
{
    report(error, {});
    const QString path = QDir(directory).filePath(QStringLiteral("settings.json"));
    if (!QFileInfo::exists(path))
        return {};
    const auto object = readObject(path, error);
    if (!object)
        return {};
    if (object->value(QStringLiteral("schemaVersion")).toInt() != 1
        || !object->value(QStringLiteral("library")).isObject()) {
        report(error, QStringLiteral("Unsupported Frameline settings format."));
        return {};
    }
    QString readError;
    const LibrarySettings library = readLibrary(object->value(QStringLiteral("library")).toObject(), &readError);
    if (!readError.isEmpty()) {
        report(error, readError);
        return {};
    }
    const HostedLibrarySettings hosted = readHostedLibrary(object->value(QStringLiteral("hostedLibrary")), &readError);
    if (!readError.isEmpty()) {
        report(error, readError);
        return {};
    }
    AppSettings settings{library, object->value(QStringLiteral("useSystemRegionShortcut")).toBool(), hosted,
                         object->value(QStringLiteral("autoCopyScreenshot")).toBool(true)};
    settings.hostedLibrary.directory = movedFromLegacyDirectory(settings.hostedLibrary.directory);
    readError = hostedConnectionError(settings);
    report(error, readError);
    return readError.isEmpty() ? settings : AppSettings{};
}

bool saveSettings(const AppSettings &settings, const QString &directory, QString *error)
{
    const auto &library = settings.library;
    const auto &hosted = settings.hostedLibrary;
    QString validation = library.validationError();
    if (validation.isEmpty())
        validation = hosted.validationError();
    if (validation.isEmpty())
        validation = hostedConnectionError(settings);
    report(error, validation);
    if (!validation.isEmpty())
        return false;
    if (!QDir().mkpath(directory)) {
        report(error, QStringLiteral("Could not create the Frameline settings folder."));
        return false;
    }
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        report(error, QStringLiteral("Could not protect the Frameline settings folder."));
        return false;
    }
#endif
    QJsonObject libraryObject{{QStringLiteral("transport"), transportName(library.transport)},
                              {QStringLiteral("sshHost"), library.sshHost},
                              {QStringLiteral("port"), library.port},
                              {QStringLiteral("token"), library.token},
                              {QStringLiteral("device"), library.device},
                              {QStringLiteral("autoArchive"), library.autoArchive}};
    if (!library.address.isEmpty())
        libraryObject.insert(QStringLiteral("address"), library.address);
    QJsonObject object{{QStringLiteral("schemaVersion"), 1},
                       {QStringLiteral("library"), libraryObject},
                       {QStringLiteral("useSystemRegionShortcut"), settings.useSystemRegionShortcut},
                       {QStringLiteral("autoCopyScreenshot"), settings.autoCopyScreenshot}};
    if (hosted != HostedLibrarySettings{}) {
        object.insert(QStringLiteral("hostedLibrary"),
                      QJsonObject{{QStringLiteral("enabled"), hosted.enabled},
                                  {QStringLiteral("directory"), hosted.directory},
                                  {QStringLiteral("token"), hosted.token},
                                  {QStringLiteral("port"), hosted.port},
                                  {QStringLiteral("share"), hosted.share}});
    }
    const QByteArray bytes = QJsonDocument(object).toJson();
    QSaveFile file(QDir(directory).filePath(QStringLiteral("settings.json")));
    if (!file.open(QIODevice::WriteOnly)
#ifndef Q_OS_WIN
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
#endif
        || file.write(bytes) != bytes.size() || !file.commit()) {
        report(error, QStringLiteral("Could not save Frameline settings. %1").arg(file.errorString()));
        return false;
    }
    return true;
}

QString libraryQueueDirectory(const LibrarySettings &settings, const QString &directory)
{
    const QString key = settings.storageKey();
    if (key.isEmpty())
        return {};
    return QDir(directory).filePath(QStringLiteral("queues/") + key);
}
