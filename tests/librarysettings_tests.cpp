#include "settings.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {

const auto fixtureToken = QStringLiteral("a0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_-z");

LibrarySettings remoteSettings(const QString &address)
{
    LibrarySettings settings;
    settings.transport = LibraryTransport::Tailscale;
    settings.address = address;
    settings.port = defaultHostedLibraryPort;
    settings.token = fixtureToken;
    settings.device = QStringLiteral("Second computer");
    return settings;
}

QString codeWithPayload(const QString &address, const QByteArray &port, const QString &token)
{
    const QByteArray payload = address.toLatin1() + '\n' + port + '\n' + token.toLatin1();
    return QStringLiteral("SF1-") + QString::fromLatin1(payload.toBase64(QByteArray::Base64UrlEncoding
                                                                        | QByteArray::OmitTrailingEquals));
}

} // namespace

class LibrarySettingsTests final : public QObject {
    Q_OBJECT

private slots:
    void connectionCodeRoundTrip();
    void invalidConnectionCode_data();
    void invalidConnectionCode();
    void queueDestinationIncludesPeer();
    void legacySettingsAndQueue();
    void hostedSettingsSurviveDisable();
    void invalidHostedSettingsDisableConnection();
};

void LibrarySettingsTests::connectionCodeRoundTrip()
{
    for (const auto &address : {QStringLiteral("100.65.12.4"), QStringLiteral("fd7a:115c:a1e0::7")}) {
        const auto original = remoteSettings(address);
        const QString code = libraryConnectionCode(original);
        QVERIFY(!code.isEmpty());
        QString error = QStringLiteral("Previous error");
        const auto imported = librarySettingsFromConnectionCode(QStringLiteral("\n ") + code + u'\n',
                                                                original.device, &error);
        QVERIFY2(imported.has_value(), qPrintable(error));
        QVERIFY(error.isEmpty());
        QVERIFY(*imported == original);

        const auto automaticDevice = librarySettingsFromConnectionCode(code, {});
        QVERIFY(automaticDevice && automaticDevice->isConfigured());
    }
}

void LibrarySettingsTests::invalidConnectionCode_data()
{
    QTest::addColumn<QString>("code");
    QTest::newRow("unsupported-version") << QStringLiteral("SF2-abcdef");
    QTest::newRow("invalid-encoding") << QStringLiteral("SF1-*abc");
    QTest::newRow("oversized") << QString(513, u'A');
    for (const auto &address : {"127.0.0.1", "192.168.1.20", "100.63.255.255", "100.128.0.0",
                                "100.65.12.004", "100.65.12.4:18746", "machine.tailnet.ts.net",
                                "::ffff:100.65.12.4", "fd7a:115c:a1e1::7", "fd7a:115c:a1e0::7%eth0"}) {
        QTest::newRow(address) << codeWithPayload(QString::fromLatin1(address), "18746", fixtureToken);
    }
    const QString address = QStringLiteral("100.65.12.4");
    QTest::newRow("port-zero") << codeWithPayload(address, "0", fixtureToken);
    QTest::newRow("port-overflow") << codeWithPayload(address, "65536", fixtureToken);
    QTest::newRow("noncanonical-port") << codeWithPayload(address, "+18746", fixtureToken);
    QTest::newRow("invalid-token") << codeWithPayload(address, "18746", fixtureToken + u' ');
    QTest::newRow("extra-field") << codeWithPayload(address, "18746", fixtureToken + u'\n');
}

void LibrarySettingsTests::invalidConnectionCode()
{
    QFETCH(QString, code);
    QString error;
    QVERIFY(!librarySettingsFromConnectionCode(code, {}, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!error.contains(fixtureToken));
}

void LibrarySettingsTests::queueDestinationIncludesPeer()
{
    auto first = remoteSettings(QStringLiteral("fd7a:115c:a1e0::7"));
    const QString key = first.storageKey();
    QVERIFY(!key.isEmpty());

    auto alias = first;
    alias.address = QStringLiteral("FD7A:115C:A1E0:0:0:0:0:7");
    QCOMPARE(alias.storageKey(), key);

    auto different = first;
    different.address = QStringLiteral("fd7a:115c:a1e0::8");
    QVERIFY(different.storageKey() != key);

    first.address.clear();
    first.transport = LibraryTransport::Local;
    QVERIFY(first.storageKey() != key);
}

void LibrarySettingsTests::legacySettingsAndQueue()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile file(directory.filePath(QStringLiteral("settings.json")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    const QByteArray legacy = R"({"schemaVersion":1,"useSystemRegionShortcut":true,"library":{
        "transport":"ssh","sshHost":"library-host","port":18745,"autoArchive":true,
        "token":"a0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_-z","device":"Existing device"}})";
    QCOMPARE(file.write(legacy), legacy.size());
    file.close();

    QString error;
    const AppSettings settings = loadSettings(directory.path(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(settings.useSystemRegionShortcut);
    QVERIFY(settings.library.isConfigured());
    QVERIFY(settings.library.autoArchive);
    QVERIFY(settings.hostedLibrary == HostedLibrarySettings{});
    QCOMPARE(settings.library.storageKey(), QStringLiteral("31a6b4209bc750b4e3885e3c30d9d0a1eb15665ea6ee6d826510aedf0bc211d0"));
}

void LibrarySettingsTests::hostedSettingsSurviveDisable()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings;
    settings.library = remoteSettings(QStringLiteral("100.65.12.4"));
    auto &hosted = settings.hostedLibrary;
    hosted.enabled = true;
    hosted.directory = defaultHostedLibraryDirectory(directory.path());
    hosted.token = generateLibraryToken();
    hosted.share = true;

    QString error;
    QVERIFY2(saveSettings(settings, directory.path(), &error), qPrintable(error));
    QVERIFY(loadSettings(directory.path(), &error) == settings);
    QVERIFY(error.isEmpty());

    hosted.enabled = false;
    QVERIFY2(saveSettings(settings, directory.path(), &error), qPrintable(error));
    QVERIFY(loadSettings(directory.path(), &error) == settings);
    QVERIFY(error.isEmpty());

    hosted.directory = QStringLiteral("relative/path");
    QVERIFY(!saveSettings(settings, directory.path(), &error));
    QVERIFY(!error.isEmpty());
    const AppSettings saved = loadSettings(directory.path(), &error);
    QVERIFY(error.isEmpty());
    QVERIFY(!saved.hostedLibrary.enabled);
    QCOMPARE(saved.hostedLibrary.directory, defaultHostedLibraryDirectory(directory.path()));
}

void LibrarySettingsTests::invalidHostedSettingsDisableConnection()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    AppSettings settings;
    settings.library.device = QStringLiteral("Host");
    settings.library.port = defaultHostedLibraryPort;
    settings.library.token = fixtureToken;
    settings.library.autoArchive = true;
    settings.hostedLibrary.enabled = true;
    settings.hostedLibrary.directory = defaultHostedLibraryDirectory(directory.path());
    settings.hostedLibrary.token = fixtureToken;
    QString error;
    QVERIFY2(saveSettings(settings, directory.path(), &error), qPrintable(error));

    QFile file(directory.filePath(QStringLiteral("settings.json")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto valid = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    const auto writeWithHostedSettings = [&](const QJsonObject &hosted) {
        auto contents = valid;
        contents[QStringLiteral("hostedLibrary")] = hosted;
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray bytes = QJsonDocument(contents).toJson();
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
    };

    auto hosted = valid.value(QStringLiteral("hostedLibrary")).toObject();
    hosted[QStringLiteral("directory")] = QStringLiteral("relative/path");
    writeWithHostedSettings(hosted);
    QVERIFY(loadSettings(directory.path(), &error) == AppSettings{});
    QVERIFY(!error.isEmpty());
    QVERIFY(loadSettings(directory.path()) == AppSettings{});

    hosted[QStringLiteral("directory")] = settings.hostedLibrary.directory;
    hosted[QStringLiteral("port")] = defaultHostedLibraryPort + 1;
    writeWithHostedSettings(hosted);
    QVERIFY(loadSettings(directory.path(), &error) == AppSettings{});
    QVERIFY(!error.isEmpty());
    settings.hostedLibrary.port = defaultHostedLibraryPort + 1;
    QVERIFY(!saveSettings(settings, directory.path(), &error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(LibrarySettingsTests)
#include "librarysettings_tests.moc"
