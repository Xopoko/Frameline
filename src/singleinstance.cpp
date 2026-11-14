#include "singleinstance.h"

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

namespace {
constexpr int activationTimeoutMs = 2500;
constexpr char acknowledged = 'K';

QString defaultRuntimeDirectory()
{
#ifdef Q_OS_WIN
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("runtime"));
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
#else
    return QDir(directory).filePath(QStringLiteral("desktop.sock"));
#endif
}

char encodeRequest(SingleInstance::Request request)
{
    switch (request) {
    case SingleInstance::Request::Editor: return 'E';
    case SingleInstance::Request::Library: return 'L';
    case SingleInstance::Request::Settings: return 'S';
    case SingleInstance::Request::Quiet: return 'Q';
    }
    return '\0';
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
    server_->setSocketOptions(QLocalServer::UserAccessOption);
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
    if (!prepareRuntimeDirectory(runtimeDirectory_))
        return fail(QStringLiteral("Could not open Frameline’s private runtime folder."));

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
            return fail(QStringLiteral("Could not start Frameline’s local connection. %1").arg(server_->errorString()));
        }
        if (lock_->error() != QLockFile::LockFailedError)
            return fail(QStringLiteral("Could not lock Frameline’s runtime folder."));

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
    return fail(QStringLiteral("Frameline is already running but could not be opened. Try again in a moment."));
}

bool SingleInstance::requestSettings()
{
    const QString directory = defaultRuntimeDirectory();
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
    if (command != 'E' && command != 'L' && command != 'S' && command != 'Q') {
        socket->abort();
        return;
    }

    disconnect(socket, nullptr, this, nullptr);
    // Let the client close after reading the reply. Closing a Windows pipe here can discard unread bytes.
    socket->write(&acknowledged, 1);
    switch (command) {
    case 'E': emit activationRequested(Request::Editor); break;
    case 'L': emit activationRequested(Request::Library); break;
    case 'S': emit activationRequested(Request::Settings); break;
    }
}
