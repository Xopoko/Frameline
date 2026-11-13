#include "libraryclient.h"
#include "libraryserver.h"
#include "tailrelay.h"

#include <QDir>
#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

class TailRelayTests final : public QObject {
    Q_OBJECT

private slots:
    void transfersInBothDirections();
    void requiresVerifiedSource();
    void stopsForwardingWhenRevoked();
    void unavailableHostKeepsQueueLocal();
};

void TailRelayTests::transfersInBothDirections()
{
    QTcpServer remote;
    remote.setProxy(QNetworkProxy::NoProxy);
    QVERIFY(remote.listen(QHostAddress::LocalHost, 0));
    TailRelay relay(QHostAddress::LocalHost, remote.serverPort(), [] { return QHostAddress::LocalHost; });
    QVERIFY(relay.listen());
    QTcpSocket client;
    client.setProxy(QNetworkProxy::NoProxy);
    client.connectToHost(QHostAddress::LocalHost, relay.port());
    QTRY_VERIFY(client.state() == QAbstractSocket::ConnectedState);
    QTRY_VERIFY(remote.hasPendingConnections());
    auto *peer = remote.nextPendingConnection();

    const QByteArray upload(512 * 1024, 'u');
    QCOMPARE(client.write(upload), upload.size());
    QTRY_COMPARE(peer->bytesAvailable(), qint64(upload.size()));
    QCOMPARE(peer->readAll(), upload);

    const QByteArray download(512 * 1024, 'd');
    QCOMPARE(peer->write(download), download.size());
    peer->disconnectFromHost();
    QTRY_VERIFY(client.state() == QAbstractSocket::UnconnectedState);
    QCOMPARE(client.readAll(), download);
}

void TailRelayTests::requiresVerifiedSource()
{
    QTcpServer remote;
    remote.setProxy(QNetworkProxy::NoProxy);
    QVERIFY(remote.listen(QHostAddress::LocalHost, 0));
    TailRelay relay(QHostAddress::LocalHost, remote.serverPort(), [] { return QHostAddress{}; });
    QVERIFY(relay.listen());
    QTcpSocket client;
    client.setProxy(QNetworkProxy::NoProxy);
    QSignalSpy closed(&client, &QTcpSocket::disconnected);
    client.connectToHost(QHostAddress::LocalHost, relay.port());
    QTRY_COMPARE(closed.size(), 1);
    QVERIFY(!remote.hasPendingConnections());
}

void TailRelayTests::stopsForwardingWhenRevoked()
{
    QTcpServer remote;
    remote.setProxy(QNetworkProxy::NoProxy);
    QVERIFY(remote.listen(QHostAddress::LocalHost, 0));
    bool allow = true;
    TailRelay relay(QHostAddress::LocalHost, remote.serverPort(), [&] {
        return allow ? QHostAddress(QHostAddress::LocalHost) : QHostAddress{};
    });
    QVERIFY(relay.listen());
    QTcpSocket client;
    client.setProxy(QNetworkProxy::NoProxy);
    client.connectToHost(QHostAddress::LocalHost, relay.port());
    QTRY_VERIFY(remote.hasPendingConnections());
    auto *peer = remote.nextPendingConnection();

    allow = false;
    relay.disconnectClients();
    QTRY_VERIFY(client.state() == QAbstractSocket::UnconnectedState);
    QTRY_VERIFY(peer->state() == QAbstractSocket::UnconnectedState);
    QCOMPARE(peer->bytesAvailable(), 0);
}

void TailRelayTests::unavailableHostKeepsQueueLocal()
{
    const QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QTcpServer otherProgram;
    otherProgram.setProxy(QNetworkProxy::NoProxy);
    QVERIFY(otherProgram.listen(QHostAddress::LocalHost, 0));

    AppSettings settings;
    settings.hostedLibrary = {true, QDir(directory.path()).filePath(QStringLiteral("library")),
                              generateLibraryToken(), otherProgram.serverPort()};
    settings.library.port = settings.hostedLibrary.port;
    settings.library.token = settings.hostedLibrary.token;
    settings.library.device = QStringLiteral("Test device");
    settings.library.autoArchive = true;
    QVERIFY(saveSettings(settings, directory.path()));

    LibraryClient client(settings.library, directory.path());
    QImage image(10, 10, QImage::Format_RGB32);
    image.fill(Qt::blue);
    client.enqueue(image, QStringLiteral("full"), QDateTime::currentDateTimeUtc());
    QVERIFY(client.flushPendingWrites());
    QCOMPARE(client.pendingCount(), 1);

    bool checked = false;
    client.list({}, this, [&](QList<LibraryItem>, const QString &, const QString &error) {
        checked = !error.isEmpty();
    });
    QTRY_VERIFY(checked);
    QCOMPARE(client.pendingCount(), 1);
    QVERIFY(!otherProgram.hasPendingConnections());

    otherProgram.close();
    LibraryServer server;
    QString error;
    QVERIFY2(server.start(settings.hostedLibrary.directory, settings.hostedLibrary.token,
                          settings.library.port, &error),
             qPrintable(error));
    client.setConnectionError(QStringLiteral("Test connection unavailable"));
    checked = false;
    client.list({}, this, [&](QList<LibraryItem>, const QString &, const QString &error) {
        checked = error == QStringLiteral("Test connection unavailable");
    });
    QTRY_VERIFY(checked);
    QCOMPARE(client.pendingCount(), 1);

    client.setConnectionError({});
    QTRY_COMPARE(client.pendingCount(), 0);
}

QTEST_GUILESS_MAIN(TailRelayTests)
#include "tailrelay_tests.moc"
