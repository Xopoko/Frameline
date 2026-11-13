#include "libraryclient.h"

#include "pngexport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrentRun>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include <algorithm>
#include <utility>

namespace {
constexpr qint64 maxImage = 64 * 1024 * 1024;
constexpr qint64 maxThumbnail = 2 * 1024 * 1024;
constexpr auto readyMarker = "screenframe-ready";

QString libraryDir()
{
    return QDir::home().filePath(QStringLiteral(".screenframe"));
}

bool isId(const QString &id)
{
    return id.size() == 36 && !QUuid(id).isNull() && QUuid(id).toString(QUuid::WithoutBraces) == id;
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return file.write(bytes) == bytes.size() && file.commit();
}

QString stageCapture(const QString &queue, const QString &id, const QImage &image, const QJsonObject &metadata)
{
    if (image.width() > 32768 || image.height() > 32768 || qint64(image.width()) * image.height() > 100'000'000)
        return QObject::tr("The screenshot is too large for the shared library.");
    if (!QDir().mkpath(queue))
        return QObject::tr("Could not create the local screenshot queue.");
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(queue, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return QObject::tr("Could not make the local screenshot queue private.");
#endif
    QTemporaryDir staging(QDir(queue).filePath(QStringLiteral(".staging-XXXXXX")));
    if (!staging.isValid())
        return QObject::tr("Could not create a local staging folder for the screenshot.");
    QString error;
    const QString original = staging.filePath(QStringLiteral("image.png"));
    if (!savePng(image, original, &error))
        return QObject::tr("Could not queue the screenshot. %1").arg(error);
    if (QFileInfo(original).size() > maxImage)
        return QObject::tr("The encoded screenshot exceeds the library's 64 MiB limit.");

    const QImage thumb = image.width() > 360 || image.height() > 240
        ? image.scaled(360, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation) : image;
    if (!savePng(thumb, staging.filePath(QStringLiteral("thumbnail.png")), &error)
        || !writeBytes(staging.filePath(QStringLiteral("metadata.json")), QJsonDocument(metadata).toJson(QJsonDocument::Compact)))
        return QObject::tr("Could not save the screenshot metadata or preview. %1").arg(error);
    if (!QDir().rename(staging.path(), QDir(queue).filePath(id)))
        return QObject::tr("Could not add the screenshot to the local queue.");
    return {};
}

QString responseError(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401 || status == 403)
        return QObject::tr("Library access was denied. Check the device's library configuration.");
    if (status == 404)
        return QObject::tr("The screenshot is no longer available.");
    if (status >= 400)
        return QObject::tr("The library server returned HTTP %1.").arg(status);
    return reply->error() == QNetworkReply::NoError ? QString{}
        : QObject::tr("Could not reach the shared library. Try again when Ubuntu is reachable.");
}

struct Download {
    QByteArray bytes;
    QString error;
};

QString sshProgram()
{
#ifdef Q_OS_WIN
    const QString system = qEnvironmentVariable("SystemRoot");
    return QDir(system).filePath(QStringLiteral("System32/OpenSSH/ssh.exe"));
#else
    return QStringLiteral("/usr/bin/ssh");
#endif
}
} // namespace

struct LibraryClient::State : QObject {
    explicit State(LibraryClient &owner)
        : owner(owner), queue(QDir(libraryDir()).filePath(QStringLiteral("queue")))
    {
        QFile file(QDir(libraryDir()).filePath(QStringLiteral("library.json")));
        if (file.open(QIODevice::ReadOnly) && file.size() <= 4096) {
            const auto config = QJsonDocument::fromJson(file.readAll()).object();
            host = config.value(QStringLiteral("sshHost")).toString();
            port = config.value(QStringLiteral("port")).toInt(18745);
            device = config.value(QStringLiteral("device")).toString();
            const QString secret = config.value(QStringLiteral("token")).toString();
            static const QRegularExpression safeHost(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\\z"));
            static const QRegularExpression safeToken(QStringLiteral("\\A[A-Za-z0-9_-]{32,256}\\z"));
            static const QRegularExpression safeDevice(QStringLiteral("\\A[ -~]{1,48}\\z"));
            if (port > 0 && port <= 65535 && (host.isEmpty() || safeHost.match(host).hasMatch())
                && safeToken.match(secret).hasMatch() && safeDevice.match(device).hasMatch()) {
                token = secret.toLatin1();
            }
        }
        network.setProxy(QNetworkProxy::NoProxy);
        writer.setMaxThreadCount(1);
        retry.setSingleShot(true);
        connect(&retry, &QTimer::timeout, this, [this] { pump(); });
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, this, [this] {
            failTunnel(tr("The private connection to Ubuntu timed out."));
            ssh.kill();
        });
        ssh.setProgram(sshProgram());
#ifdef Q_OS_WIN
        ssh.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
#endif
        connect(&ssh, &QProcess::readyReadStandardOutput, this, [this] {
            const auto bytes = ssh.readAllStandardOutput();
            if (tunnelReady)
                return;
            handshake += bytes;
            if (handshake.contains(QByteArray(readyMarker) + '\n')) {
                tunnelReady = true;
                timeout.stop();
                deliverTunnel({});
            } else if (handshake.size() > 4096) {
                failTunnel(tr("Unexpected response from the private SSH connection."));
                ssh.kill();
            }
        });
        connect(&ssh, &QProcess::readyReadStandardError, this, [this] { ssh.readAllStandardError(); });
        connect(&ssh, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                failTunnel(tr("Could not start OpenSSH for the library."));
        });
        connect(&ssh, &QProcess::finished, this, [this] {
            failTunnel(tr("Could not connect to Ubuntu with the configured SSH host."));
        });
    }

    ~State() override
    {
        QObject::disconnect(&ssh, nullptr, this, nullptr);
        if (ssh.state() != QProcess::NotRunning) {
            ssh.kill();
            ssh.waitForFinished(1000);
        }
        writer.waitForDone();
    }

    QStringList queuedIds() const
    {
        auto entries = QDir(queue).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Time | QDir::Reversed);
        entries.removeIf([](const QString &id) { return !isId(id); });
        return entries;
    }

    int pending() const
    {
        return token.isEmpty() ? 0 : static_cast<int>(writes.size() + queuedIds().size());
    }

    void status()
    {
        emit owner.syncStatusChanged(pending(), writeError.isEmpty() ? uploadError : writeError);
    }

    struct Write {
        QString id;
        QImage image;
        QJsonObject metadata;
        QFutureWatcher<QString> *watcher = nullptr;
        bool failed = false;
    };

    void stage(Write &write)
    {
        write.failed = false;
        write.watcher = new QFutureWatcher<QString>(this);
        auto *watcher = write.watcher;
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
            if (finishWrite(watcher))
                pump();
            else if (!retry.isActive())
                retry.start(15000);
        });
        watcher->setFuture(QtConcurrent::run(&writer, stageCapture, queue, write.id, write.image, write.metadata));
    }

    bool finishWrite(QFutureWatcher<QString> *watcher)
    {
        const auto found = std::find_if(writes.begin(), writes.end(), [watcher](const Write &write) { return write.watcher == watcher; });
        if (found == writes.end())
            return false;
        const QString error = watcher->result();
        watcher->disconnect(this);
        watcher->deleteLater();
        if (error.isEmpty()) {
            writes.erase(found);
        } else {
            found->watcher = nullptr;
            found->failed = true;
            writeError = error;
        }
        if (std::none_of(writes.cbegin(), writes.cend(), [](const Write &write) { return write.failed; }))
            writeError.clear();
        status();
        return error.isEmpty();
    }

    bool flush()
    {
        for (auto &write : writes) {
            if (write.failed)
                stage(write);
        }
        while (true) {
            const auto active = std::find_if(writes.cbegin(), writes.cend(), [](const Write &write) { return write.watcher; });
            if (active == writes.cend())
                break;
            auto *watcher = active->watcher;
            watcher->waitForFinished();
            finishWrite(watcher);
        }
        QTimer::singleShot(0, this, [this] { pump(); });
        return writes.isEmpty();
    }

    using Ready = std::function<void(quint16, QString)>;

    void ensureTunnel(Ready callback)
    {
        if (token.isEmpty()) {
            callback(0, tr("This device has no shared library configuration."));
            return;
        }
        if (host.isEmpty() || tunnelReady) {
            callback(host.isEmpty() ? port : localPort, {});
            return;
        }
        waiting.append(std::move(callback));
        if (ssh.state() != QProcess::NotRunning)
            return;

        QTcpServer reservation;
        if (!reservation.listen(QHostAddress::LocalHost, 0)) {
            failTunnel(tr("Could not reserve a local port for the library connection."));
            return;
        }
        localPort = reservation.serverPort();
        reservation.close();
        handshake.clear();
        ssh.setArguments({QStringLiteral("-T"), QStringLiteral("-o"), QStringLiteral("BatchMode=yes"),
                          QStringLiteral("-o"), QStringLiteral("StrictHostKeyChecking=yes"),
                          QStringLiteral("-o"), QStringLiteral("ExitOnForwardFailure=yes"),
                          QStringLiteral("-o"), QStringLiteral("ConnectTimeout=5"),
                          QStringLiteral("-o"), QStringLiteral("ServerAliveInterval=15"),
                          QStringLiteral("-o"), QStringLiteral("ServerAliveCountMax=2"),
                          QStringLiteral("-L"), QStringLiteral("127.0.0.1:%1:127.0.0.1:%2").arg(localPort).arg(port),
                          host, QStringLiteral("printf 'screenframe-ready\\n'; exec cat")});
        timeout.start(8000);
        ssh.start();
    }

    void deliverTunnel(const QString &error)
    {
        for (const auto &callback : std::exchange(waiting, {}))
            callback(error.isEmpty() ? localPort : 0, error);
    }

    void failTunnel(const QString &error)
    {
        timeout.stop();
        tunnelReady = false;
        deliverTunnel(error);
    }

    QNetworkRequest request(quint16 local, const QString &path, const QUrlQuery &query = {}) const
    {
        QUrl url;
        url.setScheme(QStringLiteral("http"));
        url.setHost(QStringLiteral("127.0.0.1"));
        url.setPort(local);
        url.setPath(path);
        url.setQuery(query);
        QNetworkRequest result(url);
        result.setRawHeader("Authorization", "Bearer " + token);
        result.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        result.setTransferTimeout(12000);
        return result;
    }

    void download(const QString &path, const QUrlQuery &query, qint64 limit, QObject *context,
                  std::function<void(Download)> callback)
    {
        const QPointer<QObject> guard(context);
        ensureTunnel([this, path, query, limit, guard, callback = std::move(callback)](quint16 local, const QString &error) {
            if (!guard)
                return;
            if (!error.isEmpty()) {
                callback({{}, error});
                return;
            }
            auto *reply = network.get(request(local, path, query));
            auto result = std::make_shared<Download>();
            auto read = [reply, result, limit] {
                if (!result->error.isEmpty())
                    return;
                result->bytes += reply->read(limit + 1 - result->bytes.size());
                if (result->bytes.size() > limit) {
                    result->bytes.clear();
                    result->error = tr("The library response exceeded the download limit.");
                    reply->abort();
                }
            };
            reply->setReadBufferSize(256 * 1024);
            connect(guard.data(), &QObject::destroyed, reply, [reply] { reply->abort(); });
            connect(reply, &QIODevice::readyRead, this, read);
            connect(reply, &QNetworkReply::finished, this, [reply, result, read, guard, callback] {
                read();
                if (result->error.isEmpty())
                    result->error = responseError(reply);
                reply->deleteLater();
                if (guard)
                    callback(std::move(*result));
            });
        });
    }

    void pump()
    {
        if (token.isEmpty() || active)
            return;
        for (auto &write : writes) {
            if (write.failed)
                stage(write);
        }
        const auto ids = queuedIds();
        if (ids.isEmpty()) {
            uploadError.clear();
            status();
            return;
        }
        for (const QString &id : ids) {
            auto lock = std::make_unique<QLockFile>(QDir(queue).filePath(id + QStringLiteral(".lock")));
            lock->setStaleLockTime(0);
            if (!lock->tryLock())
                continue;
            active = std::move(lock);
            activeId = id;
            break;
        }
        if (!active) {
            retry.start(3000);
            return;
        }
        ensureTunnel([this](quint16 local, const QString &error) {
            if (!error.isEmpty()) {
                uploadFinished(error);
                return;
            }
            upload(local, false);
        });
    }

    void upload(quint16 local, bool thumbnail)
    {
        const QDir folder(QDir(queue).filePath(activeId));
        QFile metadata(folder.filePath(QStringLiteral("metadata.json")));
        if (!metadata.open(QIODevice::ReadOnly) || metadata.size() > 4096) {
            uploadFinished(tr("A queued screenshot is missing its metadata."));
            return;
        }
        const auto object = QJsonDocument::fromJson(metadata.readAll()).object();
        const QString part = thumbnail ? QStringLiteral("thumbnail") : QStringLiteral("image");
        auto file = std::make_unique<QFile>(folder.filePath(part + QStringLiteral(".png")));
        if (!file->open(QIODevice::ReadOnly)) {
            uploadFinished(tr("A queued screenshot file could not be read."));
            return;
        }
        auto uploadRequest = request(local, QStringLiteral("/v1/screenshots/%1/%2").arg(activeId, part));
        uploadRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/png"));
        uploadRequest.setHeader(QNetworkRequest::ContentLengthHeader, file->size());
        uploadRequest.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
        if (!thumbnail) {
            uploadRequest.setRawHeader("X-ScreenFrame-Captured-At", object.value(QStringLiteral("capturedAt")).toString().toLatin1());
            uploadRequest.setRawHeader("X-ScreenFrame-Device", object.value(QStringLiteral("device")).toString().toLatin1());
            uploadRequest.setRawHeader("X-ScreenFrame-Kind", object.value(QStringLiteral("kind")).toString().toLatin1());
            uploadRequest.setRawHeader("X-ScreenFrame-Width", QByteArray::number(object.value(QStringLiteral("width")).toInt()));
            uploadRequest.setRawHeader("X-ScreenFrame-Height", QByteArray::number(object.value(QStringLiteral("height")).toInt()));
        }
        auto *reply = network.put(uploadRequest, file.get());
        auto *opened = file.release();
        opened->setParent(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, opened, local, thumbnail] {
            opened->close();
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString error = responseError(reply);
            if (error.isEmpty() && code != 201 && code != 204)
                error = tr("The library did not acknowledge the screenshot upload.");
            reply->deleteLater();
            if (error.isEmpty() && !thumbnail)
                upload(local, true);
            else
                uploadFinished(error);
        });
    }

    void uploadFinished(QString error)
    {
        if (error.isEmpty()) {
            const QString sent = QStringLiteral(".sent-") + activeId;
            if (QDir(queue).rename(activeId, sent)) {
                QDir(QDir(queue).filePath(sent)).removeRecursively();
                emit owner.libraryChanged();
            } else {
                error = tr("The screenshot was uploaded, but its local queue entry could not be removed.");
            }
        }
        active.reset();
        activeId.clear();
        uploadError = std::move(error);
        status();
        retry.start(uploadError.isEmpty() ? 0 : 15000);
    }

    LibraryClient &owner;
    const QString queue;
    QString host;
    int port = 18745;
    QString device;
    QByteArray token;
    QThreadPool writer;
    QList<Write> writes;
    QNetworkAccessManager network;
    QProcess ssh;
    QTimer timeout;
    QTimer retry;
    QByteArray handshake;
    QList<Ready> waiting;
    quint16 localPort = 0;
    bool tunnelReady = false;
    std::unique_ptr<QLockFile> active;
    QString activeId;
    QString writeError;
    QString uploadError;
};

LibraryClient::LibraryClient(QObject *parent) : QObject(parent), d(std::make_unique<State>(*this)) {}
LibraryClient::~LibraryClient() = default;

bool LibraryClient::isConfigured() const { return !d->token.isEmpty(); }
int LibraryClient::pendingCount() const { return d->pending(); }

void LibraryClient::start()
{
    if (isConfigured()) {
        d->status();
        d->pump();
        // Other ScreenFrame processes, including GNOME hotkey invocations, share the same queue.
        auto *poll = new QTimer(d.get());
        poll->setInterval(10000);
        connect(poll, &QTimer::timeout, d.get(), [this] { d->pump(); });
        poll->start();
    }
}

void LibraryClient::enqueue(QImage image, const QString &kind, const QDateTime &capturedAt)
{
    if (!isConfigured() || image.isNull())
        return;
    const QJsonObject metadata{{QStringLiteral("capturedAt"), capturedAt.toUTC().toString(Qt::ISODateWithMs)},
                               {QStringLiteral("device"), d->device}, {QStringLiteral("kind"), kind},
                               {QStringLiteral("width"), image.width()}, {QStringLiteral("height"), image.height()}};
    d->writes.append({QUuid::createUuid().toString(QUuid::WithoutBraces), std::move(image), metadata});
    d->stage(d->writes.last());
    d->status();
}

bool LibraryClient::flushPendingWrites() { return d->flush(); }

void LibraryClient::list(const QString &cursor, QObject *context, ListCallback callback)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("50"));
    if (!cursor.isEmpty())
        query.addQueryItem(QStringLiteral("cursor"), cursor);
    d->download(QStringLiteral("/v1/screenshots"), query, 256 * 1024, context,
                [callback = std::move(callback)](Download response) {
        if (!response.error.isEmpty()) {
            callback({}, {}, response.error);
            return;
        }
        const auto page = QJsonDocument::fromJson(response.bytes).object();
        if (!page.value(QStringLiteral("items")).isArray()) {
            callback({}, {}, tr("The library returned an invalid screenshot list."));
            return;
        }
        QList<LibraryItem> items;
        for (const auto &entry : page.value(QStringLiteral("items")).toArray()) {
            const auto item = entry.toObject();
            const QString id = item.value(QStringLiteral("id")).toString();
            if (!isId(id))
                continue;
            items.append({id, QDateTime::fromString(item.value(QStringLiteral("capturedAt")).toString(), Qt::ISODateWithMs),
                          {item.value(QStringLiteral("width")).toInt(), item.value(QStringLiteral("height")).toInt()},
                          item.value(QStringLiteral("device")).toString(), item.value(QStringLiteral("kind")).toString(),
                          item.value(QStringLiteral("hasThumbnail")).toBool()});
        }
        callback(std::move(items), page.value(QStringLiteral("nextCursor")).toString(), {});
    });
}

void LibraryClient::image(const QString &id, bool thumbnail, QObject *context, ImageCallback callback)
{
    if (!isId(id)) {
        callback({}, tr("Invalid screenshot ID."));
        return;
    }
    const QPointer<QObject> guard(context);
    const QString part = thumbnail ? QStringLiteral("thumbnail") : QStringLiteral("image");
    d->download(QStringLiteral("/v1/screenshots/%1/%2").arg(id, part), {}, thumbnail ? maxThumbnail : maxImage,
                context, [this, guard, callback = std::move(callback)](Download response) {
        if (!response.error.isEmpty()) {
            callback({}, response.error);
            return;
        }
        auto *decoded = new QFutureWatcher<QImage>(d.get());
        connect(decoded, &QFutureWatcher<QImage>::finished, d.get(), [decoded, guard, callback] {
            const QImage image = decoded->result();
            decoded->deleteLater();
            if (guard)
                callback(image, image.isNull() ? tr("The library image could not be decoded.") : QString{});
        });
        decoded->setFuture(QtConcurrent::run([bytes = std::move(response.bytes)] { return QImage::fromData(bytes); }));
    });
}
