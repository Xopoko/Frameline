#include "libraryclient.h"

#include "libraryprotocol.h"
#include "libraryserver.h"
#include "tailrelay.h"
#include "tailscale.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
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
#include <QSet>
#include <QTcpServer>
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
using LibraryProtocol::isScreenshotId;

// The remote end of the SSH tunnel prints this line once the forwarded port is ready.
constexpr auto readyMarker = "freezecut-ready";

QString responseError(QNetworkReply *reply)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401 || status == 403)
        return QStringLiteral("Library access was denied. Check the device's library configuration.");
    if (status == 404)
        return QStringLiteral("The screenshot is no longer available.");
    if (status >= 400)
        return QStringLiteral("The library server returned HTTP %1.").arg(status);
    return reply->error() == QNetworkReply::NoError ? QString{}
        : QStringLiteral("Could not reach the shared library. Check that the library server is running and reachable.");
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
    State(LibraryClient &owner, const LibrarySettings &settings, const QString &directory, bool localHistory = false)
        : owner(owner),
          settingsDirectory(directory), localHistory(localHistory), storageKey(settings.storageKey()),
          queue(localHistory ? QDir(directory).filePath(QStringLiteral("history"))
                             : libraryQueueDirectory(settings, settingsDirectory)),
          transport(settings.transport), peer(QHostAddress(settings.address).toString()),
          host(settings.transport == LibraryTransport::Ssh ? settings.sshHost : QString{}),
          port(settings.port), device(localHistory ? defaultLibraryDeviceName() : settings.device),
          token(settings.isConfigured() ? settings.token.toLatin1() : QByteArray{}),
          autoArchive(settings.autoArchive)
    {
        network.setProxy(QNetworkProxy::NoProxy);
        if (localHistory) {
            connect(&historyWatcher, &QFileSystemWatcher::directoryChanged, this, [this] {
                watchHistory();
                emit this->owner.libraryChanged();
            });
        }
        if (transport == LibraryTransport::Local && !token.isEmpty()) {
            const auto hosted = loadSettings(settingsDirectory).hostedLibrary;
            forHostedLibrary = hosted.token.toLatin1() == token && hosted.port == port;
        }
        if (transport == LibraryTransport::Tailscale && !token.isEmpty()) {
            tailscale = std::make_unique<TailscaleMonitor>();
            relay = std::make_unique<TailRelay>(QHostAddress(peer), port, [this] {
                return connectionError.isEmpty() ? QHostAddress(tailscale->status().sourceForPeer(peer))
                                                 : QHostAddress{};
            });
            connect(tailscale.get(), &TailscaleMonitor::refreshed, this, &State::tailscaleRefreshed);
        }
        writer.setMaxThreadCount(1);
        retry.setSingleShot(true);
        connect(&retry, &QTimer::timeout, this, [this] { pump(); });
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, this, [this] {
            failTunnel(QStringLiteral("The SSH connection to the library timed out."));
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
                failTunnel(QStringLiteral("Unexpected response from the private SSH connection."));
                ssh.kill();
            }
        });
        connect(&ssh, &QProcess::readyReadStandardError, this, [this] { ssh.readAllStandardError(); });
        connect(&ssh, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                failTunnel(QStringLiteral("Could not start OpenSSH for the library."));
        });
        connect(&ssh, &QProcess::finished, this, [this] {
            failTunnel(QStringLiteral("Could not connect to the library using the configured SSH host."));
        });
    }

    ~State() override
    {
        for (auto *reply : activeReplies)
            reply->disconnect(this);
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
        entries.removeIf([](const QString &id) { return !isScreenshotId(id); });
        return entries;
    }

    QStringList historyDirectories() const
    {
        QStringList directories{queue, defaultHostedLibraryDirectory(settingsDirectory)};
        const QDir profile(settingsDirectory);
        if (profile.dirName() == QStringLiteral(".frameline")) {
            directories.append(QDir::cleanPath(profile.filePath(QStringLiteral("../.screenframe/library"))));
        }
        if (!hostedHistoryDirectory.isEmpty())
            directories.append(hostedHistoryDirectory);
        directories.removeDuplicates();
        return directories;
    }

    void watchHistory()
    {
        QStringList paths;
        for (const QString &directory : historyDirectories()) {
            QFileInfo entry(directory);
            if (directory == queue) {
                // A different process may still be creating the history folder.
                while (!entry.isDir() && entry.absoluteFilePath() != entry.absolutePath())
                    entry = QFileInfo(entry.absolutePath());
            }
            if (entry.isDir())
                paths.append(entry.absoluteFilePath());
        }
        const auto watched = historyWatcher.directories();
        for (const QString &path : watched) {
            if (!paths.contains(path))
                historyWatcher.removePath(path);
        }
        for (const QString &path : paths) {
            if (!watched.contains(path))
                historyWatcher.addPath(path);
        }
    }

    int pending() const
    {
        if (localHistory)
            return static_cast<int>(writes.size());
        return archivingAllowed() ? static_cast<int>(writes.size() + queuedIds().size()) : 0;
    }

    bool archivingAllowed() const
    {
        if (localHistory)
            return true;
        if (!autoArchive || storageKey.isEmpty())
            return false;
        // The tray and CLI can be running at the same time with different settings snapshots.
        const auto saved = loadSettings(settingsDirectory).library;
        return saved.autoArchive && saved.storageKey() == storageKey;
    }

    void status()
    {
        const QString &error = writeError.isEmpty() ? (connectionError.isEmpty() ? uploadError : connectionError)
                                                   : writeError;
        emit owner.syncStatusChanged(pending(), error);
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
        watcher->setFuture(QtConcurrent::run(&writer, LibraryStorage::writeCapture, queue, write.id, write.image,
                                             write.metadata, !localHistory));
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
        if (localHistory && error.isEmpty())
            emit owner.libraryChanged();
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

    QString connectionProblem() const
    {
#ifdef FREEZECUT_MAC_APP_STORE
        if (transport == LibraryTransport::Ssh)
            return QStringLiteral("SSH libraries require the direct download of FreezeCut.");
#endif
        if (!connectionError.isEmpty())
            return connectionError;
        if (transport == LibraryTransport::Local) {
            QString error;
            const auto hosted = loadSettings(settingsDirectory, &error).hostedLibrary;
            if (!error.isEmpty())
                return error;
            const bool matchesHost = hosted.port == port && hosted.token.toLatin1() == token;
            if ((forHostedLibrary || matchesHost) && (!matchesHost || !hosted.enabled
                || !LibraryServer::isRunning(hosted.directory)))
                return QStringLiteral("Open FreezeCut on the computer that hosts the library.");
        }
        return {};
    }

    QString tailscaleProblem(const TailscaleStatus &status) const
    {
        if (status.state != TailscaleState::Ready)
            return status.message;
        return QStringLiteral("The library computer is not reachable in Tailscale. Check that both devices "
                              "are connected to the same Tailscale network.");
    }

    void ensureConnection(Ready callback)
    {
        if (token.isEmpty()) {
            callback(0, QStringLiteral("This device has no shared library configuration."));
            return;
        }
        const QString error = connectionProblem();
        if (!error.isEmpty()) {
            callback(0, error);
            return;
        }
        if (tailscale) {
            const auto status = tailscale->status();
            if (status.canReach(peer)) {
                if (relay->listen())
                    callback(relay->port(), {});
                else
                    callback(0, QStringLiteral("Could not open the private library connection."));
            } else {
                if (tailWaiting.isEmpty())
                    QTimer::singleShot(0, tailscale.get(), &TailscaleMonitor::refresh);
                tailWaiting.append(std::move(callback));
            }
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
            failTunnel(QStringLiteral("Could not reserve a local port for the library connection."));
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
                          host, QStringLiteral("printf '%1\\n'; exec cat").arg(QLatin1String(readyMarker))});
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

    void abortRequests()
    {
        const QPointer<State> guard(this);
        const auto inFlight = activeReplies.values();
        for (auto *reply : inFlight) {
            // A finished handler may replace this client.
            if (!guard)
                return;
            if (activeReplies.contains(reply))
                reply->abort();
        }
    }

    void tailscaleRefreshed(const TailscaleStatus &status)
    {
        const auto source = status.sourceForPeer(peer);
        const QPointer<State> guard(this);
        if (source != tailSource || source.isEmpty()) {
            tailSource = source;
            relay->disconnectClients();
            abortRequests();
            if (!guard)
                return;
            network.clearConnectionCache();
        }

        const QString blocked = connectionProblem();
        QString error = blocked;
        if (error.isEmpty() && source.isEmpty())
            error = tailscaleProblem(status);
        if (!tailWaiting.isEmpty() && error.isEmpty() && !relay->listen())
            error = QStringLiteral("Could not open the private library connection.");
        const quint16 local = error.isEmpty() ? relay->port() : 0;
        for (const auto &callback : std::exchange(tailWaiting, {})) {
            if (!guard)
                return;
            callback(local, error);
        }
        if (guard && error.isEmpty() && !active && !uploadError.isEmpty()) {
            retry.stop();
            pump();
        }
    }

    void track(QNetworkReply *reply)
    {
        activeReplies.insert(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply] { activeReplies.remove(reply); });
    }

    QString requestProblem() const
    {
        const QString error = connectionProblem();
        if (!error.isEmpty())
            return error;
        if (tailscale) {
            const auto status = tailscale->status();
            if (!status.canReach(peer))
                return tailscaleProblem(status);
        }
        return {};
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
        if (tailscale)
            result.setRawHeader("Connection", "close");
        result.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        result.setTransferTimeout(12000);
        return result;
    }

    void download(const QString &path, const QUrlQuery &query, qint64 limit, QObject *context,
                  std::function<void(Download)> callback)
    {
        const QPointer<QObject> guard(context);
        ensureConnection([this, path, query, limit, guard, callback = std::move(callback)](quint16 local, QString error) {
            if (!guard)
                return;
            if (error.isEmpty())
                error = requestProblem();
            if (!error.isEmpty()) {
                callback({{}, error});
                return;
            }
            auto *reply = network.get(request(local, path, query));
            track(reply);
            auto result = std::make_shared<Download>();
            auto read = [reply, result, limit] {
                if (!result->error.isEmpty())
                    return;
                result->bytes += reply->read(limit + 1 - result->bytes.size());
                if (result->bytes.size() > limit) {
                    result->bytes.clear();
                    result->error = QStringLiteral("The library response exceeded the download limit.");
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
        if (active || !archivingAllowed())
            return;
        for (auto &write : writes) {
            if (write.failed)
                stage(write);
        }
        if (localHistory)
            return;
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
        ensureConnection([this](quint16 local, const QString &error) {
            if (!error.isEmpty()) {
                uploadFinished(error);
                return;
            }
            upload(local, false);
        });
    }

    void upload(quint16 local, bool thumbnail)
    {
        if (!archivingAllowed()) {
            active.reset();
            activeId.clear();
            status();
            return;
        }
        const QString error = requestProblem();
        if (!error.isEmpty()) {
            uploadFinished(error);
            return;
        }
        const QDir folder(QDir(queue).filePath(activeId));
        QFile metadata(folder.filePath(QStringLiteral("metadata.json")));
        if (!metadata.open(QIODevice::ReadOnly) || metadata.size() > 4096) {
            uploadFinished(QStringLiteral("A queued screenshot is missing its metadata."));
            return;
        }
        const auto object = QJsonDocument::fromJson(metadata.readAll()).object();
        const QString part = thumbnail ? QStringLiteral("thumbnail") : QStringLiteral("image");
        auto file = std::make_unique<QFile>(folder.filePath(part + QStringLiteral(".png")));
        if (!file->open(QIODevice::ReadOnly)) {
            uploadFinished(QStringLiteral("A queued screenshot file could not be read."));
            return;
        }
        auto uploadRequest = request(local, QStringLiteral("/v1/screenshots/%1/%2").arg(activeId, part));
        uploadRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("image/png"));
        uploadRequest.setHeader(QNetworkRequest::ContentLengthHeader, file->size());
        uploadRequest.setAttribute(QNetworkRequest::DoNotBufferUploadDataAttribute, true);
        if (!thumbnail) {
            // The header names predate the rename from ScreenFrame; libraries already running expect them.
            uploadRequest.setRawHeader("X-ScreenFrame-Captured-At", object.value(QStringLiteral("capturedAt")).toString().toLatin1());
            uploadRequest.setRawHeader("X-ScreenFrame-Device", object.value(QStringLiteral("device")).toString().toLatin1());
            uploadRequest.setRawHeader("X-ScreenFrame-Kind", object.value(QStringLiteral("kind")).toString().toLatin1());
            uploadRequest.setRawHeader("X-ScreenFrame-Width", QByteArray::number(object.value(QStringLiteral("width")).toInt()));
            uploadRequest.setRawHeader("X-ScreenFrame-Height", QByteArray::number(object.value(QStringLiteral("height")).toInt()));
        }
        auto *reply = network.put(uploadRequest, file.get());
        track(reply);
        auto *opened = file.release();
        opened->setParent(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, opened, local, thumbnail] {
            opened->close();
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString error = responseError(reply);
            if (error.isEmpty() && code != 201 && code != 204)
                error = QStringLiteral("The library did not acknowledge the screenshot upload.");
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
                error = QStringLiteral("The screenshot was uploaded, but its local queue entry could not be removed.");
            }
        }
        active.reset();
        activeId.clear();
        uploadError = std::move(error);
        status();
        retry.start(uploadError.isEmpty() ? 0 : 15000);
    }

    LibraryClient &owner;
    const QString settingsDirectory;
    const bool localHistory;
    const QString storageKey;
    const QString queue;
    const LibraryTransport transport;
    const QString peer;
    const QString host;
    const int port;
    const QString device;
    const QByteArray token;
    const bool autoArchive;
    // This client was created for the library that FreezeCut hosts on this computer.
    bool forHostedLibrary = false;
    QString hostedHistoryDirectory;
    QFileSystemWatcher historyWatcher;
    QThreadPool writer;
    QList<Write> writes;
    QNetworkAccessManager network;
    std::unique_ptr<TailscaleMonitor> tailscale;
    std::unique_ptr<TailRelay> relay;
    QString tailSource;
    QList<Ready> tailWaiting;
    QSet<QNetworkReply *> activeReplies;
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
    QString connectionError;
};

LibraryClient::LibraryClient(const LibrarySettings &settings, const QString &settingsDirectory)
    : d(std::make_unique<State>(*this, settings, settingsDirectory)) {}
LibraryClient::LibraryClient(const QString &settingsDirectory, const QString &hostedDirectory)
    : d(std::make_unique<State>(*this, LibrarySettings{}, settingsDirectory, true))
{
    d->hostedHistoryDirectory = hostedDirectory;
}
LibraryClient::~LibraryClient() = default;

bool LibraryClient::isConfigured() const { return isLocalHistory() || !d->token.isEmpty(); }
bool LibraryClient::isLocalHistory() const { return d->localHistory; }
int LibraryClient::pendingCount() const { return d->pending(); }

void LibraryClient::setHostedHistoryDirectory(const QString &directory)
{
    if (!isLocalHistory() || d->hostedHistoryDirectory == directory)
        return;
    d->hostedHistoryDirectory = directory;
    d->watchHistory();
    emit libraryChanged();
}

void LibraryClient::start()
{
    if (isLocalHistory()) {
        d->watchHistory();
        return;
    }
    if (d->tailscale)
        QTimer::singleShot(0, d->tailscale.get(), &TailscaleMonitor::refresh);
    if (isConfigured() && d->autoArchive) {
        d->status();
        d->pump();
        // Other FreezeCut processes, including GNOME hotkey invocations, share the same queue.
        auto *poll = new QTimer(d.get());
        poll->setInterval(10000);
        connect(poll, &QTimer::timeout, d.get(), [this] { d->pump(); });
        poll->start();
    }
}

void LibraryClient::setConnectionError(const QString &error)
{
    if (d->connectionError == error)
        return;
    d->connectionError = error;
    const QPointer<State> guard(d.get());
    if (!error.isEmpty()) {
        if (d->relay)
            d->relay->disconnectClients();
        d->abortRequests();
        if (!guard)
            return;
        d->network.clearConnectionCache();
    } else {
        d->uploadError.clear();
        QTimer::singleShot(0, d.get(), [state = d.get()] { state->pump(); });
    }
    d->status();
}

QString LibraryClient::enqueue(QImage image, const QString &kind, const QDateTime &capturedAt, const QString &id)
{
    if (image.isNull() || !d->archivingAllowed())
        return {};
    const QString captureId = id.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : id;
    const QJsonObject metadata{{QStringLiteral("capturedAt"), capturedAt.toUTC().toString(Qt::ISODateWithMs)},
                               {QStringLiteral("device"), d->device}, {QStringLiteral("kind"), kind},
                               {QStringLiteral("width"), image.width()}, {QStringLiteral("height"), image.height()}};
    d->writes.append({captureId, std::move(image), metadata});
    d->stage(d->writes.last());
    d->status();
    return captureId;
}

bool LibraryClient::flushPendingWrites() { return d->flush(); }

void LibraryClient::list(const QString &cursor, QObject *context, ListCallback callback)
{
    if (isLocalHistory()) {
        auto *watcher = new QFutureWatcher<LibraryStorage::Page>(d.get());
        const QPointer<QObject> guard(context);
        connect(watcher, &QFutureWatcher<LibraryStorage::Page>::finished, d.get(), [watcher, guard, callback] {
            auto page = watcher->result();
            watcher->deleteLater();
            if (guard)
                callback(std::move(page.items), std::move(page.nextCursor), std::move(page.error));
        });
        watcher->setFuture(QtConcurrent::run(LibraryStorage::list, d->historyDirectories(), cursor));
        return;
    }
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
            callback({}, {}, QStringLiteral("The library returned an invalid screenshot list."));
            return;
        }
        QList<LibraryItem> items;
        for (const auto &entry : page.value(QStringLiteral("items")).toArray()) {
            const auto item = entry.toObject();
            const QString id = item.value(QStringLiteral("id")).toString();
            if (!isScreenshotId(id))
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
    if (!isScreenshotId(id)) {
        callback({}, QStringLiteral("Invalid screenshot ID."));
        return;
    }
    const QPointer<QObject> guard(context);
    if (isLocalHistory()) {
        auto *watcher = new QFutureWatcher<LibraryStorage::Image>(d.get());
        connect(watcher, &QFutureWatcher<LibraryStorage::Image>::finished, d.get(), [watcher, guard, callback] {
            auto result = watcher->result();
            watcher->deleteLater();
            if (guard)
                callback(std::move(result.image), std::move(result.error));
        });
        watcher->setFuture(QtConcurrent::run(LibraryStorage::image, d->historyDirectories(), id, thumbnail));
        return;
    }
    const QString part = thumbnail ? QStringLiteral("thumbnail") : QStringLiteral("image");
    const qint64 limit = thumbnail ? LibraryProtocol::maxThumbnailBytes : LibraryProtocol::maxImageBytes;
    d->download(QStringLiteral("/v1/screenshots/%1/%2").arg(id, part), {}, limit, context,
                [this, guard, callback = std::move(callback)](Download response) {
        if (!response.error.isEmpty()) {
            callback({}, response.error);
            return;
        }
        auto *decoded = new QFutureWatcher<QImage>(d.get());
        connect(decoded, &QFutureWatcher<QImage>::finished, d.get(), [decoded, guard, callback] {
            const QImage image = decoded->result();
            decoded->deleteLater();
            if (guard)
                callback(image, image.isNull() ? QStringLiteral("The library image could not be decoded.") : QString{});
        });
        decoded->setFuture(QtConcurrent::run([bytes = std::move(response.bytes)] { return QImage::fromData(bytes); }));
    });
}
