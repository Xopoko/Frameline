#pragma once

#include <QHostAddress>

#include <functional>
#include <memory>

bool pinTailInterface(qintptr socket, const QHostAddress &source);

// The HTTP client connects on loopback. Only the relay opens remote sockets,
// and it binds each one to the verified source address and interface.
class TailRelay final {
public:
    using SourceAddress = std::function<QHostAddress()>;

    TailRelay(QHostAddress peer, quint16 peerPort, SourceAddress source);
    ~TailRelay();

    bool listen();
    quint16 port() const;
    void disconnectClients();

private:
    struct State;
    std::unique_ptr<State> d;
};
