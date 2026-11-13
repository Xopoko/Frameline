#include "tailrelay.h"

#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#if defined(Q_OS_WIN)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#elif defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

#include <utility>

namespace {
constexpr qint64 bufferSize = 128 * 1024;
constexpr int maxConnections = 8;
constexpr int idleTimeoutMs = 15000;
} // namespace

bool pinTailInterface(qintptr socket, const QHostAddress &source)
{
#if defined(Q_OS_WIN)
    const bool v6 = source.protocol() == QAbstractSocket::IPv6Protocol;
    MIB_UNICASTIPADDRESS_TABLE *addresses = nullptr;
    if (GetUnicastIpAddressTable(v6 ? AF_INET6 : AF_INET, &addresses) != NO_ERROR)
        return false;
    DWORD index = 0;
    for (ULONG i = 0; i < addresses->NumEntries; ++i) {
        const auto &entry = addresses->Table[i];
        if (QHostAddress(reinterpret_cast<const sockaddr *>(&entry.Address)) != source)
            continue;
        if (index && index != entry.InterfaceIndex) {
            index = 0;
            break;
        }
        index = entry.InterfaceIndex;
    }
    FreeMibTable(addresses);
    if (!index)
        return false;
    const DWORD option = v6 ? index : htonl(index);
    return setsockopt(static_cast<SOCKET>(socket), v6 ? IPPROTO_IPV6 : IPPROTO_IP,
                      v6 ? IPV6_UNICAST_IF : IP_UNICAST_IF,
                      reinterpret_cast<const char *>(&option), sizeof(option)) == 0;
#elif defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    QNetworkInterface match;
    for (const auto &interface : QNetworkInterface::allInterfaces()) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp))
            continue;
        for (const auto &entry : interface.addressEntries()) {
            if (entry.ip() != source)
                continue;
            if (match.isValid() && match.index() != interface.index())
                return false;
            match = interface;
        }
    }
    if (!match.isValid())
        return false;
#  if defined(Q_OS_MACOS)
    const bool v6 = source.protocol() == QAbstractSocket::IPv6Protocol;
    const auto option = static_cast<unsigned int>(match.index());
    return setsockopt(static_cast<int>(socket), v6 ? IPPROTO_IPV6 : IPPROTO_IP,
                      v6 ? IPV6_BOUND_IF : IP_BOUND_IF, &option, sizeof(option)) == 0;
#  else
    // Initial device binding on a new socket needs no capability on supported Linux kernels.
    const QByteArray name = match.name().toLocal8Bit();
    return setsockopt(static_cast<int>(socket), SOL_SOCKET, SO_BINDTODEVICE,
                      name.constData(), static_cast<socklen_t>(name.size() + 1)) == 0;
#  endif
#else
    return false;
#endif
}

namespace {
class RelayStream final : public QObject {
public:
    RelayStream(QTcpSocket &client, QObject *parent) : QObject(parent), client(client)
    {
        client.setParent(this);
        client.setReadBufferSize(bufferSize);
        remote.setReadBufferSize(bufferSize);
        remote.setProxy(QNetworkProxy::NoProxy);
        timeout.setSingleShot(true);
        timeout.setInterval(idleTimeoutMs);
        connect(&timeout, &QTimer::timeout, this, &RelayStream::close);
        connect(&client, &QTcpSocket::disconnected, this, &RelayStream::close);
        connect(&remote, &QTcpSocket::disconnected, this, [this] { forwardResponse(); });
        connect(&client, &QTcpSocket::readyRead, this, [this] { forwardRequest(); });
        connect(&remote, &QTcpSocket::bytesWritten, this, [this] { forwardRequest(); });
        connect(&remote, &QTcpSocket::readyRead, this, [this] { forwardResponse(); });
        connect(&client, &QTcpSocket::bytesWritten, this, [this] { forwardResponse(); });
        connect(&remote, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
            if (error != QAbstractSocket::RemoteHostClosedError)
                close();
        });
    }

    ~RelayStream() override
    {
        client.disconnect(this);
        remote.disconnect(this);
    }

    void start(const QHostAddress &source, const QHostAddress &peer, quint16 port)
    {
        if (source.isNull() || source.protocol() != peer.protocol()
            || !remote.bind(source, 0) || !pinTailInterface(remote.socketDescriptor(), source)) {
            close();
            return;
        }
        connect(&remote, &QTcpSocket::connected, this, [this, source, peer] {
            if (remote.localAddress() != source || remote.peerAddress() != peer) {
                close();
                return;
            }
            forwardRequest();
        });
        timeout.start();
        remote.connectToHost(peer, port);
    }

    void close()
    {
        if (closed)
            return;
        closed = true;
        timeout.stop();
        client.abort();
        remote.abort();
        deleteLater();
    }

private:
    void forward(QTcpSocket &from, QTcpSocket &to)
    {
        if (closed || to.state() != QAbstractSocket::ConnectedState)
            return;
        const qint64 size = qMin(from.bytesAvailable(), bufferSize - to.bytesToWrite());
        if (size <= 0)
            return;
        const QByteArray bytes = from.read(size);
        if (to.write(bytes) != bytes.size()) {
            close();
            return;
        }
        timeout.start();
    }

    void forwardRequest() { forward(client, remote); }

    void forwardResponse()
    {
        forward(remote, client);
        if (!closed && remote.state() == QAbstractSocket::UnconnectedState && !remote.bytesAvailable())
            client.disconnectFromHost();
    }

    QTcpSocket &client;
    QTcpSocket remote;
    QTimer timeout;
    bool closed = false;
};
} // namespace

struct TailRelay::State : QObject {
    State(QHostAddress peer, quint16 port, SourceAddress source)
        : peer(std::move(peer)), peerPort(port), source(std::move(source))
    {
        listener.setProxy(QNetworkProxy::NoProxy);
        listener.setMaxPendingConnections(maxConnections);
        connect(&listener, &QTcpServer::newConnection, this, [this] { accept(); });
    }

    ~State() override
    {
        // QObject destroys the child streams after this destructor. Disconnect the
        // removal callbacks before destroying the set they access.
        for (auto *stream : streams)
            stream->disconnect(this);
    }

    void accept()
    {
        while (auto *client = listener.nextPendingConnection()) {
            if (streams.size() >= maxConnections) {
                client->abort();
                client->deleteLater();
                continue;
            }
            auto *stream = new RelayStream(*client, this);
            streams.insert(stream);
            connect(stream, &QObject::destroyed, this, [this, stream] { streams.remove(stream); });
            stream->start(source(), peer, peerPort);
        }
    }

    QTcpServer listener;
    QSet<RelayStream *> streams;
    const QHostAddress peer;
    const quint16 peerPort;
    const SourceAddress source;
};

TailRelay::TailRelay(QHostAddress peer, quint16 peerPort, SourceAddress source)
    : d(std::make_unique<State>(std::move(peer), peerPort, std::move(source))) {}

TailRelay::~TailRelay() = default;

bool TailRelay::listen()
{
    return d->listener.isListening() || d->listener.listen(QHostAddress::LocalHost, 0);
}

quint16 TailRelay::port() const { return d->listener.serverPort(); }

void TailRelay::disconnectClients()
{
    for (auto *stream : d->streams)
        stream->close();
}
