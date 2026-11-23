#include "libraryserver.h"

#include "libraryprotocol.h"
#include "tailrelay.h"
#include "tailscale.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QNetworkProxy>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QtEndian>

#include <algorithm>
#include <utility>

namespace {
constexpr qint64 maxHeaders = 16 * 1024;
constexpr qint64 chunkSize = 64 * 1024;
constexpr int maxConnections = 8;
constexpr int headerTimeout = 10000;
constexpr int transferTimeout = 60000;
const auto imageRoot = QStringLiteral("/v1/screenshots");
const auto originalName = QStringLiteral("image.png");
const auto metadataName = QStringLiteral("metadata.json");
const auto lockName = QStringLiteral(".server.lock");
const QByteArray pngType = "image/png";
const QByteArray jpegType = "image/jpeg";

using Headers = QHash<QByteArray, QByteArray>;
using OrderKey = QPair<QString, QString>;
using LibraryProtocol::isImageSize;
using LibraryProtocol::isScreenshotId;
using LibraryProtocol::maxImageBytes;
using LibraryProtocol::maxThumbnailBytes;

bool report(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return message.isEmpty();
}

bool sameSecret(const QByteArray &candidate, const QByteArray &secret)
{
    if (candidate.size() != secret.size())
        return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < secret.size(); ++i)
        difference |= static_cast<unsigned char>(candidate[i] ^ secret[i]);
    return difference == 0;
}

bool digits(const QByteArray &value, qsizetype maxLength)
{
    return !value.isEmpty() && value.size() <= maxLength
        && std::all_of(value.cbegin(), value.cend(), [](char c) { return c >= '0' && c <= '9'; });
}

QString normalizedTime(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral(
        "\\A[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(?:\\.[0-9]+)?(?:Z|[+-][0-9]{2}:[0-9]{2})\\z"));
    if (!pattern.match(value).hasMatch())
        return {};
    const auto time = QDateTime::fromString(value, Qt::ISODateWithMs);
    return time.isValid() ? time.toUTC().toString(Qt::ISODateWithMs) : QString{};
}

bool validDevice(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[ -~]{1,64}\\z"));
    return pattern.match(value).hasMatch() && !value.trimmed().isEmpty();
}

bool validKind(const QString &value)
{
    return value == QStringLiteral("full") || value == QStringLiteral("region");
}

QString thumbnailName(const QByteArray &type)
{
    return type == pngType ? QStringLiteral("thumbnail.png") : QStringLiteral("thumbnail.jpg");
}

QString encodeCursor(const OrderKey &key)
{
    const auto bytes = (key.first + u'/' + key.second).toLatin1();
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool decodeCursor(const QString &value, OrderKey &key)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{1,128}\\z"));
    if (!pattern.match(value).hasMatch())
        return false;
    const auto decoded = QByteArray::fromBase64Encoding(value.toLatin1(), QByteArray::Base64UrlEncoding
                                                        | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return false;
    const auto parts = QString::fromLatin1(decoded.decoded).split(u'/');
    if (parts.size() != 2 || parts[0].isEmpty() || normalizedTime(parts[0]) != parts[0] || !isScreenshotId(parts[1]))
        return false;
    key = {parts[0], parts[1]};
    return true;
}

bool completePng(QFile &file)
{
    if (!file.seek(0) || file.read(8) != QByteArray::fromHex("89504e470d0a1a0a"))
        return false;
    bool hasPixels = false;
    const qint64 total = file.size();
    for (int i = 0; i < 16384 && total - file.pos() >= 12; ++i) {
        const QByteArray header = file.read(8);
        if (header.size() != 8)
            return false;
        const auto length = qFromBigEndian<quint32>(header.constData());
        const QByteArray type = header.sliced(4);
        if (i == 0 && (type != "IHDR" || length != 13))
            return false;
        if (type == "IEND")
            return hasPixels && length == 0 && total - file.pos() == 4 && file.read(4) == QByteArray::fromHex("ae426082");
        if (length > total - file.pos() - 4 || !file.seek(file.pos() + length + 4))
            return false;
        hasPixels |= type == "IDAT" && length > 0;
    }
    return false;
}

QSize imageSize(QFile &file, const QByteArray &type)
{
    if (type == pngType) {
        if (!completePng(file))
            return {};
    } else if (file.read(2) != QByteArray::fromHex("ffd8") || !file.seek(file.size() - 2)
               || file.read(2) != QByteArray::fromHex("ffd9")) {
        return {};
    }
    if (!file.seek(0))
        return {};
    QImageReader reader(&file, type == pngType ? "png" : "jpeg");
    return reader.canRead() ? reader.size() : QSize{};
}

bool writeMetadata(const QString &path, const QJsonObject &metadata)
{
    QFile file(path);
    const QByteArray bytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size() && file.flush();
}

QByteArray reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    default: return "Internal Server Error";
    }
}
} // namespace

struct LibraryServer::State {
    struct Record {
        QJsonObject metadata;
        QByteArray imageHash;
        QByteArray thumbnailType;
    };

    struct Connection final : QObject {
        Connection(State &server, QTcpSocket *socket, bool shared);

        void readRequest();
        void readHeader();
        void route(const QByteArray &method, const QUrl &url, const Headers &headers);
        void beginUpload(const Headers &headers);
        void readUpload();
        void finishUpload();
        void sendImage();
        void sendList(const QUrlQuery &query);
        void sendHeader(int status, qint64 length, const QByteArray &type = {});
        void sendJson(int status, const QJsonObject &object);
        void fail(int status, const QString &message);
        void pumpDownload();

        State &server;
        QTcpSocket *socket;
        bool shared;
        QTimer deadline;
        enum class Phase { Header, Upload, Response } phase = Phase::Header;
        QString id;
        bool thumbnail = false;
        QByteArray type;
        QJsonObject metadata;
        qint64 remaining = 0;
        QCryptographicHash uploadHash{QCryptographicHash::Sha256};
        std::unique_ptr<QTemporaryDir> staging;
        QFile upload;
        QFile download;
    };

    State()
    {
        local.setProxy(QNetworkProxy::NoProxy);
        share.setProxy(QNetworkProxy::NoProxy);
        local.setMaxPendingConnections(maxConnections);
        share.setMaxPendingConnections(maxConnections);
        QObject::connect(&local, &QTcpServer::newConnection, &local, [this] { accept(local, false); });
        QObject::connect(&share, &QTcpServer::newConnection, &share, [this] { accept(share, true); });
    }

    void accept(QTcpServer &listener, bool shared)
    {
        while (auto *socket = listener.nextPendingConnection()) {
            const auto peer = socket->peerAddress();
            const bool allowed = shared ? isTailscaleAddress(peer) && authorizePeer
                                             && authorizePeer(socket->localAddress().toString(), peer.toString())
                                             && pinTailInterface(socket->socketDescriptor(), socket->localAddress())
                                        : peer.isLoopback();
            if (!allowed || connections.size() >= maxConnections) {
                socket->abort();
                socket->deleteLater();
                continue;
            }
            auto *connection = new Connection(*this, socket, shared);
            connections.insert(connection);
            QObject::connect(connection, &QObject::destroyed, &local, [this, connection] { connections.remove(connection); });
            connection->readRequest();
        }
    }

    void closeConnections(bool sharedOnly)
    {
        const auto active = connections;
        for (auto *connection : active) {
            if (!sharedOnly || connection->shared)
                delete connection;
        }
    }

    void loadRecords()
    {
        static const QRegularExpression stagingPattern(QStringLiteral("\\A\\.incoming-[A-Za-z0-9]{6}\\z"));
        const auto entries = data.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden);
        for (const auto &entry : entries) {
            const QString id = entry.fileName();
            if (stagingPattern.match(id).hasMatch()) {
                QDir(entry.filePath()).removeRecursively();
                continue;
            }
            if (!isScreenshotId(id))
                continue;
            const QDir capture(entry.filePath());
            const QFileInfo image(capture.filePath(originalName));
            const QFileInfo meta(capture.filePath(metadataName));
            QFile metadataFile(meta.filePath());
            if (!image.isFile() || image.isSymLink() || !meta.isFile() || meta.isSymLink() || meta.size() > 2048
                || !metadataFile.open(QIODevice::ReadOnly))
                continue;
            auto object = QJsonDocument::fromJson(metadataFile.readAll()).object();
            const auto hash = QByteArray::fromHex(object.take(QStringLiteral("imageSha256")).toString().toLatin1());
            const auto time = object.value(QStringLiteral("capturedAt")).toString();
            if (time.isEmpty() || normalizedTime(time) != time || hash.size() != 32
                || !isImageSize({object.value(QStringLiteral("width")).toInt(), object.value(QStringLiteral("height")).toInt()})
                || !validDevice(object.value(QStringLiteral("device")).toString())
                || !validKind(object.value(QStringLiteral("kind")).toString()))
                continue;
            QByteArray thumbnailType;
            for (const auto &candidate : {pngType, jpegType}) {
                const QFileInfo thumbnail(capture.filePath(thumbnailName(candidate)));
                if (thumbnail.isFile() && !thumbnail.isSymLink()) {
                    thumbnailType = candidate;
                    break;
                }
            }
            order.insert({time, id}, id);
            records.insert(id, {object, hash, thumbnailType});
        }
    }

    QTcpServer local;
    QTcpServer share;
    QSet<Connection *> connections;
    QDir data;
    PeerAuthorizer authorizePeer;
    QByteArray authorization;
    std::unique_ptr<QLockFile> lock;
    QHash<QString, Record> records;
    QMap<OrderKey, QString> order;
};

LibraryServer::State::Connection::Connection(State &server, QTcpSocket *socket, bool shared)
    : server(server), socket(socket), shared(shared)
{
    socket->setParent(this);
    socket->setReadBufferSize(chunkSize);
    connect(socket, &QTcpSocket::readyRead, this, &Connection::readRequest);
    connect(socket, &QTcpSocket::disconnected, this, &QObject::deleteLater);
    connect(socket, &QTcpSocket::bytesWritten, this, [this] { pumpDownload(); });
    deadline.setSingleShot(true);
    connect(&deadline, &QTimer::timeout, this, [this] {
        this->socket->abort();
        deleteLater();
    });
    deadline.start(headerTimeout);
}

void LibraryServer::State::Connection::readRequest()
{
    if (phase == Phase::Header)
        readHeader();
    if (phase == Phase::Upload)
        readUpload();
}

void LibraryServer::State::Connection::readHeader()
{
    const QByteArray header = socket->peek(maxHeaders + 1);
    const qsizetype end = header.indexOf("\r\n\r\n");
    if (end < 0 && header.size() <= maxHeaders)
        return;
    if (end < 0 || end + 4 > maxHeaders) {
        fail(431, QStringLiteral("Request headers exceed the limit."));
        return;
    }
    socket->read(end + 4);
    const auto lines = header.first(end).split('\n');
    QByteArray firstLine = lines.first();
    if (firstLine.endsWith('\r'))
        firstLine.chop(1);
    const auto request = firstLine.split(' ');
    if (request.size() != 3 || (request[2] != "HTTP/1.0" && request[2] != "HTTP/1.1")
        || !request[1].startsWith('/') || request[1].startsWith("//")) {
        fail(400, QStringLiteral("Invalid request line."));
        return;
    }
    const auto url = QUrl::fromEncoded(request[1], QUrl::StrictMode);
    if (!url.isValid() || !url.isRelative() || url.hasFragment()) {
        fail(400, QStringLiteral("Invalid request URL."));
        return;
    }
    Headers headers;
    static const QRegularExpression namePattern(QStringLiteral("\\A[!#$%&'*+.^_`|~0-9A-Za-z-]+\\z"));
    for (qsizetype i = 1; i < lines.size(); ++i) {
        QByteArray line = lines[i];
        if (line.endsWith('\r'))
            line.chop(1);
        const qsizetype colon = line.indexOf(':');
        if (colon < 1) {
            fail(400, QStringLiteral("Invalid request header."));
            return;
        }
        const auto name = line.first(colon).toLower();
        const auto value = line.sliced(colon + 1).trimmed();
        if (!namePattern.match(QString::fromLatin1(name)).hasMatch() || headers.contains(name)
            || !std::all_of(value.cbegin(), value.cend(), [](char c) { return c == '\t' || (c >= ' ' && c <= '~'); })) {
            fail(400, QStringLiteral("Invalid request header."));
            return;
        }
        headers.insert(name, value);
    }
    if (headers.contains("transfer-encoding")) {
        fail(400, QStringLiteral("Transfer-Encoding is unsupported."));
        return;
    }
    route(request[0], url, headers);
}

void LibraryServer::State::Connection::route(const QByteArray &method, const QUrl &url, const Headers &headers)
{
    if (method != "GET" && method != "PUT") {
        fail(405, QStringLiteral("Request method is unsupported."));
        return;
    }
    if (method == "GET" && headers.contains("content-length") && headers.value("content-length") != "0") {
        fail(400, QStringLiteral("GET requests cannot have a body."));
        return;
    }
    const auto path = url.path(QUrl::FullyEncoded);
    if (method == "GET" && path == QStringLiteral("/healthz")) {
        sendJson(200, {{QStringLiteral("status"), QStringLiteral("ok")}});
        return;
    }
    if (path != imageRoot && !path.startsWith(imageRoot + u'/')) {
        fail(404, QStringLiteral("Endpoint does not exist."));
        return;
    }
    if (!sameSecret(headers.value("authorization"), server.authorization)) {
        fail(401, QStringLiteral("Authorization required."));
        return;
    }
    if (path == imageRoot) {
        if (method == "GET")
            sendList(QUrlQuery(url));
        else
            fail(405, QStringLiteral("Request method is unsupported."));
        return;
    }
    const auto parts = path.sliced(imageRoot.size() + 1).split(u'/');
    if (parts.size() != 2 || (parts[1] != QStringLiteral("image") && parts[1] != QStringLiteral("thumbnail"))) {
        fail(404, QStringLiteral("Endpoint does not exist."));
        return;
    }
    id = parts[0];
    if (!isScreenshotId(id)) {
        fail(400, QStringLiteral("Invalid screenshot ID."));
        return;
    }
    thumbnail = parts[1] == QStringLiteral("thumbnail");
    if (method == "GET")
        sendImage();
    else
        beginUpload(headers);
}

void LibraryServer::State::Connection::beginUpload(const Headers &headers)
{
    type = headers.value("content-type");
    if (type != pngType && (!thumbnail || type != jpegType)) {
        fail(415, QStringLiteral("Unsupported image type."));
        return;
    }
    const auto length = headers.value("content-length");
    if (!digits(length, 20)) {
        fail(411, QStringLiteral("Content-Length required."));
        return;
    }
    bool parsed = false;
    remaining = length.toLongLong(&parsed);
    if (!parsed || remaining <= 0 || remaining > (thumbnail ? maxThumbnailBytes : maxImageBytes)) {
        fail(413, QStringLiteral("Image exceeds allowed size."));
        return;
    }
    if (thumbnail && !server.records.contains(id)) {
        fail(404, QStringLiteral("Screenshot does not exist."));
        return;
    }
    if (!thumbnail) {
        // The header names predate the rename from ScreenFrame, so every client version can upload.
        const auto time = normalizedTime(QString::fromLatin1(headers.value("x-screenframe-captured-at")));
        const auto width = headers.value("x-screenframe-width");
        const auto height = headers.value("x-screenframe-height");
        const auto device = QString::fromLatin1(headers.value("x-screenframe-device"));
        const auto kind = QString::fromLatin1(headers.value("x-screenframe-kind"));
        const QSize size(width.toInt(), height.toInt());
        if (time.isEmpty() || !digits(width, 5) || !digits(height, 5) || !isImageSize(size)
            || !validDevice(device) || !validKind(kind)) {
            fail(400, QStringLiteral("Invalid screenshot metadata."));
            return;
        }
        metadata = {{QStringLiteral("capturedAt"), time}, {QStringLiteral("width"), size.width()},
                    {QStringLiteral("height"), size.height()}, {QStringLiteral("device"), device},
                    {QStringLiteral("kind"), kind}};
    }
    const auto expect = headers.value("expect").toLower();
    if (!expect.isEmpty() && expect != "100-continue") {
        fail(417, QStringLiteral("Expectation is unsupported."));
        return;
    }
    staging = std::make_unique<QTemporaryDir>(server.data.filePath(QStringLiteral(".incoming-XXXXXX")));
    if (!staging->isValid()) {
        fail(500, QStringLiteral("Library storage failed."));
        return;
    }
    upload.setFileName(staging->filePath(originalName));
    if (!upload.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        fail(500, QStringLiteral("Library storage failed."));
        return;
    }
    phase = Phase::Upload;
    deadline.start(transferTimeout);
    if (expect == "100-continue")
        socket->write("HTTP/1.1 100 Continue\r\n\r\n");
}

void LibraryServer::State::Connection::readUpload()
{
    while (remaining > 0 && socket->bytesAvailable() > 0) {
        const QByteArray chunk = socket->read(std::min(remaining, chunkSize));
        if (chunk.isEmpty() || upload.write(chunk) != chunk.size()) {
            fail(500, QStringLiteral("Library storage failed."));
            return;
        }
        uploadHash.addData(chunk);
        remaining -= chunk.size();
    }
    if (remaining == 0) {
        if (socket->bytesAvailable() > 0)
            fail(400, QStringLiteral("Unexpected data after the image."));
        else
            finishUpload();
    }
}

void LibraryServer::State::Connection::finishUpload()
{
    if (!upload.flush()) {
        fail(500, QStringLiteral("Library storage failed."));
        return;
    }
    upload.close();
    QFile staged(upload.fileName());
    if (!staged.open(QIODevice::ReadOnly)) {
        fail(500, QStringLiteral("Library storage failed."));
        return;
    }
    const auto size = imageSize(staged, type);
    if (!isImageSize(size) || (thumbnail && std::max(size.width(), size.height()) > 1024)
        || (!thumbnail && size != QSize(metadata.value(QStringLiteral("width")).toInt(),
                                        metadata.value(QStringLiteral("height")).toInt()))) {
        fail(400, QStringLiteral("Invalid image or mismatched dimensions."));
        return;
    }
    const QByteArray digest = uploadHash.result();
    auto record = server.records.find(id);
    const QDir capture(server.data.filePath(id));
    if (thumbnail) {
        if (!record->thumbnailType.isEmpty()) {
            QFile previous(capture.filePath(thumbnailName(record->thumbnailType)));
            QCryptographicHash previousHash(QCryptographicHash::Sha256);
            if (type != record->thumbnailType || !previous.open(QIODevice::ReadOnly)
                || !previousHash.addData(&previous) || previousHash.result() != digest) {
                fail(409, QStringLiteral("Thumbnail already exists."));
                return;
            }
            sendHeader(204, 0);
        } else {
            QSaveFile target(capture.filePath(thumbnailName(type)));
            if (!staged.seek(0) || !target.open(QIODevice::WriteOnly)) {
                fail(500, QStringLiteral("Library storage failed."));
                return;
            }
            const auto bytes = staged.readAll();
            if (bytes.size() != staged.size() || target.write(bytes) != bytes.size() || !target.commit()) {
                fail(500, QStringLiteral("Library storage failed."));
                return;
            }
            record->thumbnailType = type;
            sendHeader(201, 0);
        }
    } else if (record != server.records.end()) {
        if (record->metadata != metadata || record->imageHash != digest) {
            fail(409, QStringLiteral("Screenshot already exists."));
            return;
        }
        sendHeader(204, 0);
    } else {
        staged.close();
        QJsonObject stored = metadata;
        stored.insert(QStringLiteral("imageSha256"), QString::fromLatin1(digest.toHex()));
        if (!writeMetadata(staging->filePath(metadataName), stored) || !QDir().rename(staging->path(), capture.path())) {
            fail(500, QStringLiteral("Library storage failed."));
            return;
        }
        staging->setAutoRemove(false);
        server.records.insert(id, {metadata, digest, {}});
        server.order.insert({metadata.value(QStringLiteral("capturedAt")).toString(), id}, id);
        sendHeader(201, 0);
    }
    socket->disconnectFromHost();
}

void LibraryServer::State::Connection::sendImage()
{
    const auto record = server.records.constFind(id);
    if (record == server.records.cend() || (thumbnail && record->thumbnailType.isEmpty())) {
        fail(404, QStringLiteral("Image does not exist."));
        return;
    }
    const QDir capture(server.data.filePath(id));
    type = thumbnail ? record->thumbnailType : pngType;
    const QFileInfo file(capture.filePath(thumbnail ? thumbnailName(type) : originalName));
    download.setFileName(file.filePath());
    if (!file.isFile() || file.isSymLink() || !download.open(QIODevice::ReadOnly)) {
        fail(404, QStringLiteral("Image does not exist."));
        return;
    }
    const auto length = download.size();
    if (length <= 0 || length > (thumbnail ? maxThumbnailBytes : maxImageBytes)) {
        fail(500, QStringLiteral("Library storage failed."));
        return;
    }
    remaining = length;
    sendHeader(200, length, type);
    pumpDownload();
}

void LibraryServer::State::Connection::sendList(const QUrlQuery &query)
{
    int limit = 50;
    QSet<QString> seen;
    OrderKey cursor;
    for (const auto &[key, value] : query.queryItems(QUrl::FullyDecoded)) {
        if (seen.contains(key) || (key != QStringLiteral("limit") && key != QStringLiteral("cursor"))) {
            fail(400, QStringLiteral("Invalid list parameters."));
            return;
        }
        seen.insert(key);
        if (key == QStringLiteral("limit")) {
            limit = value.toInt();
            if (!digits(value.toLatin1(), 3) || limit < 1 || limit > 100) {
                fail(400, QStringLiteral("Limit must be between 1 and 100."));
                return;
            }
        } else if (!decodeCursor(value, cursor)) {
            fail(400, QStringLiteral("Invalid cursor."));
            return;
        }
    }
    const auto &order = std::as_const(server.order);
    auto it = cursor.first.isEmpty() ? order.cend() : order.lowerBound(cursor);
    QJsonArray items;
    while (it != order.cbegin() && items.size() < limit) {
        --it;
        const auto &record = server.records.constFind(it.value()).value();
        auto item = record.metadata;
        item.insert(QStringLiteral("id"), it.value());
        item.insert(QStringLiteral("hasThumbnail"), !record.thumbnailType.isEmpty());
        items.append(item);
    }
    const QJsonValue next = it == order.cbegin() ? QJsonValue(QJsonValue::Null) : QJsonValue(encodeCursor(it.key()));
    sendJson(200, {{QStringLiteral("items"), items}, {QStringLiteral("nextCursor"), next}});
}

void LibraryServer::State::Connection::sendHeader(int status, qint64 length, const QByteArray &type)
{
    phase = Phase::Response;
    deadline.start(transferTimeout);
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason(status) + "\r\n";
    if (status != 204)
        response += "Content-Length: " + QByteArray::number(length) + "\r\n";
    if (!type.isEmpty())
        response += "Content-Type: " + type + "\r\n";
    if (status == 401)
        response += "WWW-Authenticate: Bearer\r\n";
    response += "Connection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n";
    socket->write(response);
}

void LibraryServer::State::Connection::sendJson(int status, const QJsonObject &object)
{
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    sendHeader(status, bytes.size(), "application/json");
    socket->write(bytes);
    socket->disconnectFromHost();
}

void LibraryServer::State::Connection::fail(int status, const QString &message)
{
    upload.close();
    download.close();
    sendJson(status, {{QStringLiteral("error"), message}});
}

void LibraryServer::State::Connection::pumpDownload()
{
    if (!download.isOpen() || socket->bytesToWrite() >= chunkSize)
        return;
    const auto bytes = download.read(std::min(remaining, chunkSize));
    if (bytes.isEmpty() || socket->write(bytes) != bytes.size()) {
        socket->abort();
        return;
    }
    remaining -= bytes.size();
    if (remaining == 0) {
        download.close();
        socket->disconnectFromHost();
    }
}

LibraryServer::LibraryServer(QObject *parent) : QObject(parent), d(std::make_unique<State>()) {}

LibraryServer::~LibraryServer() { stop(); }

bool LibraryServer::start(const QString &dataDir, const QString &token, quint16 port, QString *error)
{
    if (d->local.isListening())
        return report(error, QStringLiteral("The library is already running."));
    if (!LibraryProtocol::isToken(token))
        return report(error, QStringLiteral("The library access token is invalid."));
    if (dataDir.isEmpty() || !QDir::isAbsolutePath(dataDir) || QDir(dataDir).isRoot()
        || !QDir().mkpath(dataDir) || QFileInfo(dataDir).isSymLink())
        return report(error, QStringLiteral("Could not create the library folder."));
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(dataDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return report(error, QStringLiteral("Could not protect the library folder."));
#endif
    d->data = QDir(dataDir);
    auto lock = std::make_unique<QLockFile>(d->data.filePath(lockName));
    lock->setStaleLockTime(0);
    if (!lock->tryLock(0)) {
        return report(error, lock->error() == QLockFile::LockFailedError
                                 ? QStringLiteral("Another Frameline is running this library.")
                                 : QStringLiteral("Could not lock the library folder."));
    }
    if (!d->local.listen(QHostAddress::LocalHost, port))
        return report(error, QStringLiteral("Could not start the library. %1").arg(d->local.errorString()));
    d->lock = std::move(lock);
    d->authorization = "Bearer " + token.toLatin1();
    d->loadRecords();
    return report(error, {});
}

void LibraryServer::stop()
{
    d->share.close();
    d->local.close();
    d->closeConnections(false);
    d->authorization.clear();
    d->records.clear();
    d->order.clear();
    d->lock.reset();
}

quint16 LibraryServer::port() const
{
    return d->local.isListening() ? d->local.serverPort() : 0;
}

bool LibraryServer::isRunning(const QString &dataDir)
{
    if (dataDir.isEmpty() || !QFileInfo(dataDir).isDir())
        return false;
    QLockFile lock(QDir(dataDir).filePath(lockName));
    lock.setStaleLockTime(0);
    return !lock.tryLock(0) && lock.error() == QLockFile::LockFailedError;
}

void LibraryServer::setPeerAuthorizer(PeerAuthorizer authorizer)
{
    d->authorizePeer = std::move(authorizer);
    d->closeConnections(true);
}

bool LibraryServer::setShareAddress(const QString &address, QString *error)
{
    if (!address.isEmpty() && d->share.isListening() && QHostAddress(address) == d->share.serverAddress())
        return report(error, {});
    d->share.close();
    d->closeConnections(true);
    if (address.isEmpty())
        return report(error, {});
    const QHostAddress host(address);
    if (!port() || !d->authorizePeer || !isTailscaleAddress(host))
        return report(error, QStringLiteral("Connect Tailscale on this computer to share the library."));
    if (!d->share.listen(host, port()))
        return report(error, QStringLiteral("Could not share the library on Tailscale. %1").arg(d->share.errorString()));
    return report(error, {});
}

QString LibraryServer::shareAddress() const
{
    return d->share.isListening() ? d->share.serverAddress().toString() : QString{};
}
