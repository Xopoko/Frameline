#include "singleinstance.h"

#ifdef FREEZECUT_MAC_APP_STORE
#include "platform/macos_paths.h"
#endif

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <array>
#include <optional>
#include <utility>

namespace {
constexpr int activationTimeoutMs = 2500;
constexpr char acknowledged = 'K';

QString defaultRuntimeDirectory()
{
#ifdef Q_OS_WIN
    // Share the runtime with releases published as Frameline.
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("Frameline/Frameline/runtime"));
#elif defined(FREEZECUT_MAC_APP_STORE)
    // Keep this endpoint inside the Store container, with a short path for sockaddr_un.
    const QString temp = macSandboxTemporaryDirectory();
    const QString directory = QDir(temp).filePath(QStringLiteral("fc"));
    if (!temp.isEmpty() && QFile::encodeName(directory + QStringLiteral("/s")).size() < sizeof(sockaddr_un::sun_path))
        return directory;
    return {};
#else
    const QString name = QStringLiteral("frameline-%1").arg(geteuid());
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString directory = QDir(runtime).filePath(name);
    if (!runtime.isEmpty()
        && QFile::encodeName(directory + QStringLiteral("/desktop.sock")).size() < sizeof(sockaddr_un::sun_path)) {
        return directory;
    }
    return QDir(QStringLiteral("/tmp")).filePath(name);
#endif
}

bool prepareRuntimeDirectory(const QString &directory)
{
#ifdef Q_OS_WIN
    return QDir().mkpath(directory);
#else
    // Some systems ignore permissions on the socket itself. Restrict access to its directory.
    const QByteArray path = QFile::encodeName(directory);
    mkdir(path.constData(), 0700);
    struct stat info {};
    return lstat(path.constData(), &info) == 0 && S_ISDIR(info.st_mode)
        && info.st_uid == geteuid() && (info.st_mode & 0777) == 0700;
#endif
}

QString endpointName(const QString &directory)
{
#ifdef Q_OS_WIN
    const QByteArray id = QCryptographicHash::hash(directory.toCaseFolded().toUtf8(), QCryptographicHash::Sha256);
    return QStringLiteral("Frameline-%1").arg(QString::fromLatin1(id.toHex().left(32)));
#elif defined(FREEZECUT_MAC_APP_STORE)
    return QDir(directory).filePath(QStringLiteral("s"));
#else
    return QDir(directory).filePath(QStringLiteral("desktop.sock"));
#endif
}

// The byte a launching process sends to the running one for each request.
constexpr std::array<std::pair<SingleInstance::Request, char>, 5> requestCodes{{
    {SingleInstance::Request::Editor, 'E'},
    {SingleInstance::Request::Library, 'L'},
    {SingleInstance::Request::Settings, 'S'},
    {SingleInstance::Request::Quiet, 'Q'},
    {SingleInstance::Request::Welcome, 'W'},
}};

char encodeRequest(SingleInstance::Request request)
{
    for (const auto &[candidate, code] : requestCodes) {
        if (candidate == request)
            return code;
    }
    return '\0';
}

std::optional<SingleInstance::Request> decodeRequest(char code)
{
    for (const auto &[request, candidate] : requestCodes) {
        if (candidate == code)
            return request;
    }
    return {};
}

void allowActivation(QLocalSocket &socket, SingleInstance::Request request)
{
#ifdef Q_OS_WIN
    if (request == SingleInstance::Request::Quiet)
        return;
    // A foreground launch can give the running instance permission to raise its window.
    ULONG pid = 0;
    if (GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(socket.socketDescriptor()), &pid))
        AllowSetForegroundWindow(pid);
#else
    Q_UNUSED(socket);
    Q_UNUSED(request);
#endif
}

bool forwardRequest(QLocalSocket &socket, SingleInstance::Request request, int timeout)
{
    allowActivation(socket, request);
    const QDeadlineTimer deadline(timeout);
    const char command = encodeRequest(request);
    return socket.write(&command, 1) == 1
        && (socket.bytesToWrite() == 0 || socket.waitForBytesWritten(int(deadline.remainingTime())))
        && (socket.bytesAvailable() || socket.waitForReadyRead(int(deadline.remainingTime())))
        && socket.read(1) == QByteArray(1, acknowledged);
}
} // namespace

SingleInstance::SingleInstance(const QString &runtimeDirectory, QObject *parent)
    : QObject(parent), runtimeDirectory_(runtimeDirectory.isEmpty() ? defaultRuntimeDirectory() : runtimeDirectory),
      server_(std::make_unique<QLocalServer>())
{
#ifndef FREEZECUT_MAC_APP_STORE
    server_->setSocketOptions(QLocalServer::UserAccessOption);
#else
    // The containing directory is already mode 0700. Qt's socket option would
    // bind an auxiliary path longer than the sandbox's Unix socket name needs.
#endif
    connect(server_.get(), &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = server_->nextPendingConnection()) {
            socket->setReadBufferSize(1);
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] { readRequest(socket); });
            QTimer::singleShot(activationTimeoutMs, socket, &QLocalSocket::abort);
            readRequest(socket);
        }
    });
}

SingleInstance::~SingleInstance() = default;

SingleInstance::Result SingleInstance::start(Request request, QString *error)
{
    if (error)
        error->clear();
    if (server_->isListening())
        return Result::Primary;

    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return Result::Failed;
    };
    if (runtimeDirectory_.isEmpty() || !prepareRuntimeDirectory(runtimeDirectory_))
        return fail(QStringLiteral("Could not open FreezeCut’s private runtime folder."));

    const QString name = endpointName(runtimeDirectory_);
    lock_ = std::make_unique<QLockFile>(QDir(runtimeDirectory_).filePath(QStringLiteral("desktop.lock")));
    lock_->setStaleLockTime(0);
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        if (lock_->tryLock(0)) {
            QLocalServer::removeServer(name);
            if (server_->listen(name))
                return Result::Primary;
            lock_->unlock();
            return fail(QStringLiteral("Could not start FreezeCut’s local connection. %1").arg(server_->errorString()));
        }
        if (lock_->error() != QLockFile::LockFailedError)
            return fail(QStringLiteral("Could not lock FreezeCut’s runtime folder."));

        QLocalSocket socket;
        socket.setReadBufferSize(1);
        socket.connectToServer(name);
        if (socket.waitForConnected(100)) {
            if (forwardRequest(socket, request, activationTimeoutMs))
                return Result::Forwarded;
            break;
        }
        QThread::msleep(30);
    } while (elapsed.elapsed() < activationTimeoutMs);
    return fail(QStringLiteral("FreezeCut is already running but could not be opened. Try again in a moment."));
}

bool SingleInstance::requestSettings()
{
    const QString directory = defaultRuntimeDirectory();
    if (directory.isEmpty())
        return false;
    QLockFile lock(QDir(directory).filePath(QStringLiteral("desktop.lock")));
    qint64 pid = 0;
    if (!lock.getLockInfo(&pid, nullptr, nullptr) || pid == QCoreApplication::applicationPid())
        return false;
    QLocalSocket socket;
    socket.setReadBufferSize(1);
    socket.connectToServer(endpointName(directory));
    return socket.waitForConnected(100) && forwardRequest(socket, Request::Settings, activationTimeoutMs);
}

void SingleInstance::readRequest(QLocalSocket *socket)
{
    char command = 0;
    if (socket->state() != QLocalSocket::ConnectedState || socket->read(&command, 1) != 1)
        return;
    const auto request = decodeRequest(command);
    if (!request) {
        socket->abort();
        return;
    }

    disconnect(socket, nullptr, this, nullptr);
    // Let the client close after reading the reply. Closing a Windows pipe here can discard unread bytes.
    socket->write(&acknowledged, 1);
    if (*request != Request::Quiet) {
        // A modal window can process the socket's disconnect before readyRead returns.
        QMetaObject::invokeMethod(this, [this, pending = *request] {
            emit activationRequested(pending);
        }, Qt::QueuedConnection);
    }
}
