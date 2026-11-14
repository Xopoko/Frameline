#include "libraryserver.h"

#include <QBuffer>
#include <QEventLoop>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

namespace {
const auto token = QStringLiteral("frameline-test-token-123456789012");
const auto root = QStringLiteral("/v1/screenshots");
const auto firstId = QStringLiteral("11111111-1111-1111-1111-111111111111");
const auto secondId = QStringLiteral("22222222-2222-2222-2222-222222222222");
const auto firstImage = root + u'/' + firstId + QStringLiteral("/image");
const auto firstThumbnail = root + u'/' + firstId + QStringLiteral("/thumbnail");
const auto secondImage = root + u'/' + secondId + QStringLiteral("/image");

QByteArray picture(QColor color, const char *format = "PNG", QSize size = {12, 8})
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(color);
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    return image.save(&buffer, format) ? buffer.data() : QByteArray{};
}

struct Response {
    int code;
    QByteArray body;

    QJsonObject json() const { return QJsonDocument::fromJson(body).object(); }
};

QByteArray rawRequest(quint16 port, const QByteArray &request)
{
    QTcpSocket socket;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::connected, &socket, [&] { socket.write(request); });
    socket.connectToHost(QHostAddress::LocalHost, port);
    timeout.start(5000);
    loop.exec();
    return socket.readAll();
}

class Requests {
public:
    explicit Requests(quint16 port) : base(QStringLiteral("http://127.0.0.1:%1").arg(port))
    {
        manager.setProxy(QNetworkProxy::NoProxy);
    }

    Response get(const QString &path, const QString &secret = token)
    {
        return response(manager.get(request(path, secret)));
    }

    Response put(const QString &path, const QByteArray &bytes, const QByteArray &type = "image/png",
                 const QByteArray &width = "12")
    {
        auto upload = request(path, token);
        upload.setHeader(QNetworkRequest::ContentTypeHeader, type);
        upload.setHeader(QNetworkRequest::ContentLengthHeader, bytes.size());
        upload.setRawHeader("X-ScreenFrame-Captured-At", "2026-11-03T12:45:00+01:00");
        upload.setRawHeader("X-ScreenFrame-Width", width);
        upload.setRawHeader("X-ScreenFrame-Height", "8");
        upload.setRawHeader("X-ScreenFrame-Device", "Test laptop");
        upload.setRawHeader("X-ScreenFrame-Kind", "region");
        return response(manager.put(upload, bytes));
    }

private:
    QNetworkRequest request(const QString &path, const QString &secret)
    {
        QNetworkRequest request(QUrl(base + path));
        if (!secret.isEmpty())
            request.setRawHeader("Authorization", "Bearer " + secret.toLatin1());
        return request;
    }

    Response response(QNetworkReply *reply)
    {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        if (!reply->isFinished()) {
            timeout.start(5000);
            loop.exec();
        }
        const Response result{reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll()};
        delete reply;
        return result;
    }

    QString base;
    QNetworkAccessManager manager;
};

class LibraryServerTests final : public QObject {
    Q_OBJECT

private slots:
    void onlyOneServerPerFolder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString error;
        const auto path = directory.filePath(QStringLiteral("library"));
        QVERIFY(!LibraryServer::isRunning(path));
        LibraryServer first, second;
        QVERIFY2(first.start(path, token, 0, &error), qPrintable(error));
        QVERIFY(LibraryServer::isRunning(path));
        QVERIFY(!second.start(path, token, 0, &error));
        QVERIFY(!first.setShareAddress(QStringLiteral("0.0.0.0"), &error));
        QVERIFY(first.shareAddress().isEmpty());
        QCOMPARE(Requests(first.port()).get(QStringLiteral("/healthz"), {}).code, 200);
        first.stop();
        QVERIFY(!LibraryServer::isRunning(path));
        QVERIFY2(second.start(path, token, 0, &error), qPrintable(error));
    }

    void uploadBrowseAndRestart()
    {
        QTemporaryDir directory;
        LibraryServer server;
        QString error;
        QVERIFY(directory.isValid());
        QVERIFY2(server.start(directory.path(), token, 0, &error), qPrintable(error));
        Requests client(server.port());
        const auto original = picture(Qt::blue);
        const auto thumbnail = picture(Qt::blue, "JPEG");
        QVERIFY(!original.isEmpty());
        QVERIFY(!thumbnail.isEmpty());
        QCOMPARE(client.get(root, {}).code, 401);
        QCOMPARE(client.put(firstThumbnail, thumbnail, "image/jpeg").code, 404);
        QCOMPARE(client.put(firstImage, original).code, 201);
        QCOMPARE(client.put(firstImage, original).code, 204);
        QCOMPARE(client.put(firstImage, picture(Qt::red)).code, 409);
        QCOMPARE(client.put(firstThumbnail, thumbnail, "image/jpeg").code, 201);
        QCOMPARE(client.put(firstThumbnail, thumbnail, "image/jpeg").code, 204);
        QCOMPARE(client.put(firstThumbnail, picture(Qt::red)).code, 409);
        QCOMPARE(client.put(secondImage, original).code, 201);

        const auto page = client.get(root + QStringLiteral("?limit=1")).json();
        const auto items = page.value(QStringLiteral("items")).toArray();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().toObject().value(QStringLiteral("id")).toString(), secondId);
        const auto next = page.value(QStringLiteral("nextCursor")).toString();
        QVERIFY(!next.isEmpty());
        const auto older = client.get(root + QStringLiteral("?limit=1&cursor=") + next).json();
        QCOMPARE(older.value(QStringLiteral("nextCursor")).type(), QJsonValue::Null);
        const auto item = older.value(QStringLiteral("items")).toArray().first().toObject();
        QCOMPARE(item.value(QStringLiteral("id")).toString(), firstId);
        QCOMPARE(item.value(QStringLiteral("capturedAt")).toString(), QStringLiteral("2026-11-03T11:45:00.000Z"));
        QVERIFY(item.value(QStringLiteral("hasThumbnail")).toBool());

        server.stop();
        QVERIFY2(server.start(directory.path(), token, 0, &error), qPrintable(error));
        Requests restarted(server.port());
        QCOMPARE(restarted.get(firstImage).body, original);
        QCOMPARE(restarted.get(firstThumbnail).body, thumbnail);
        QCOMPARE(restarted.put(firstImage, original).code, 204);
        QCOMPARE(restarted.put(firstThumbnail, thumbnail, "image/jpeg").code, 204);
        QCOMPARE(restarted.get(root).json().value(QStringLiteral("items")).toArray().size(), 2);
    }

    void rejectsInvalidRequestsBeforePublishing()
    {
        QTemporaryDir directory;
        LibraryServer server;
        QString error;
        QVERIFY(directory.isValid());
        QVERIFY2(server.start(directory.path(), token, 0, &error), qPrintable(error));
        Requests client(server.port());
        const auto original = picture(Qt::blue);
        QCOMPARE(client.put(firstImage, original, "image/png", "13").code, 400);
        QCOMPARE(client.put(firstImage, original.chopped(12)).code, 400);
        QCOMPARE(client.put(firstImage, QByteArray("not an image")).code, 400);
        QCOMPARE(client.put(firstImage, original, "text/html").code, 415);
        QCOMPARE(client.get(firstImage).code, 404);
        QCOMPARE(client.get(root + QStringLiteral("?cursor=broken")).code, 400);
        QCOMPARE(client.get(root + QStringLiteral("?limit=1&limit=2")).code, 400);
        const auto auth = " HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer " + token.toLatin1();
        const auto traversal = rawRequest(server.port(), "GET " + root.toLatin1() + "/%2e%2e/image" + auth + "\r\n\r\n");
        QVERIFY(traversal.startsWith("HTTP/1.1 400 "));
        const auto oversized = rawRequest(server.port(), "PUT " + firstImage.toLatin1() + auth
                                          + "\r\nContent-Type: image/png\r\nContent-Length: 67108865\r\n\r\n");
        QVERIFY(oversized.startsWith("HTTP/1.1 413 "));
        QCOMPARE(client.get(firstImage).code, 404);
    }
};
} // namespace

QTEST_GUILESS_MAIN(LibraryServerTests)
#include "libraryserver_tests.moc"
