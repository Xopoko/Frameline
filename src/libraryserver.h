#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class LibraryServer final : public QObject {
public:
    explicit LibraryServer(QObject *parent = nullptr);
    ~LibraryServer() override;

    bool start(const QString &dataDir, const QString &token, quint16 port = 0, QString *error = nullptr);
    void stop();
    quint16 port() const;
    static bool isRunning(const QString &dataDir);

    // An empty address disables sharing. The caller obtains the address from Tailscale.
    using PeerAuthorizer = std::function<bool(const QString &localAddress, const QString &peerAddress)>;
    void setPeerAuthorizer(PeerAuthorizer authorizer);
    bool setShareAddress(const QString &address, QString *error = nullptr);
    QString shareAddress() const;

private:
    struct State;
    std::unique_ptr<State> d;
};
