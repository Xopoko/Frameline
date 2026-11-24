#include "controls.h"
#include "settings.h"
#include "settingswindow.h"
#include "tailscale.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QtTest>

namespace {

Button *buttonWithText(SettingsWindow &window, const QString &text)
{
    for (auto *button : window.findChildren<Button *>()) {
        if (button->text() == text)
            return button;
    }
    return nullptr;
}

} // namespace

class SettingsWindowTests : public QObject {
    Q_OBJECT

private slots:
    void captureOptionsPersist();
    void hostAndShare();
    void verifyNewConnectionBeforeSaving();
};

void SettingsWindowTests::captureOptionsPersist()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const AppSettings defaults = loadSettings(directory.path());
    QVERIFY(defaults.showEditorAfterCapture && defaults.autoCopyScreenshot);

    QFile legacy(directory.filePath(QStringLiteral("settings.json")));
    QVERIFY(legacy.open(QIODevice::WriteOnly));
    const QByteArray legacySettings = R"({"schemaVersion":1,"library":{"transport":"local"},"autoCopyScreenshot":false})";
    QCOMPARE(legacy.write(legacySettings), legacySettings.size());
    legacy.close();

    QString loadError;
    const auto loaded = loadSettings(directory.path(), &loadError);
    QVERIFY2(loadError.isEmpty(), qPrintable(loadError));
    SettingsWindow window(loaded);
    auto *preview = window.findChild<Toggle *>(QStringLiteral("showEditorAfterCapture"));
    auto *copy = window.findChild<Toggle *>(QStringLiteral("autoCopyScreenshot"));
    auto *save = buttonWithText(window, QStringLiteral("Save"));
    QVERIFY(preview && copy && save);
    QVERIFY(preview->isChecked());
    QVERIFY(!copy->isChecked());

    connect(&window, &SettingsWindow::saveRequested, this, [&](const AppSettings &settings, bool) {
        QString error;
        QVERIFY2(saveSettings(settings, directory.path(), &error), qPrintable(error));
        window.setSaveResult({}, settings);
    });
    preview->click();
    copy->click();
    save->click();
    QCOMPARE(window.result(), int(QDialog::Accepted));

    SettingsWindow reopened(loadSettings(directory.path()));
    preview = reopened.findChild<Toggle *>(QStringLiteral("showEditorAfterCapture"));
    copy = reopened.findChild<Toggle *>(QStringLiteral("autoCopyScreenshot"));
    QVERIFY(preview && !preview->isChecked());
    QVERIFY(copy && copy->isChecked());
}

void SettingsWindowTests::hostAndShare()
{
    SettingsWindow window(AppSettings{});
    window.show();
    auto *mode = window.findChild<Segmented *>(QStringLiteral("libraryConnection"));
    auto *sharing = window.findChild<Toggle *>(QStringLiteral("libraryShare"));
    auto *startup = window.findChild<Toggle *>(QStringLiteral("startOnLogin"));
    auto *copy = buttonWithText(window, QStringLiteral("Copy connection code"));
    QVERIFY(mode && sharing && startup && copy);

    TailscaleStatus status;
    status.state = TailscaleState::Ready;
    status.localAddress = QStringLiteral("100.64.1.2");
    window.setTailscaleStatus(status);

    AppSettings applied;
    bool startOnLogin = false;
    int saves = 0;
    connect(&window, &SettingsWindow::saveRequested, this, [&](const AppSettings &settings, bool autostart) {
        ++saves;
        applied = settings;
        startOnLogin = autostart;
        // The controller can choose another port when initially creating the library.
        applied.hostedLibrary.port = 29101;
        applied.library.port = applied.hostedLibrary.port;
        window.setSaveResult({}, applied);
    });

    mode->setCurrent(1);
    startup->setChecked(true);
    auto *create = buttonWithText(window, QStringLiteral("Create shared library"));
    QVERIFY(create);
    QVERIFY(!copy->isVisible());
    create->click();
    QCOMPARE(saves, 1);
    QVERIFY(startOnLogin);
    QVERIFY(applied.hostedLibrary.enabled);
    QVERIFY(!applied.hostedLibrary.share);
    QVERIFY(!applied.hostedLibrary.directory.isEmpty());
    QVERIFY(applied.library.isConfigured());
    QVERIFY(applied.library.autoArchive);
    QCOMPARE(applied.library.token, applied.hostedLibrary.token);
    QVERIFY(window.isVisible());
    window.setHostedStatus(true);

    sharing->setChecked(true);
    auto *apply = buttonWithText(window, QStringLiteral("Apply sharing"));
    QVERIFY(apply);
    QVERIFY(!copy->isVisible());
    apply->click();
    QCOMPARE(saves, 2);
    QVERIFY(applied.hostedLibrary.share);
    QVERIFY(window.isVisible());
    QVERIFY(!copy->isVisible());

    window.setHostedStatus(true, status.localAddress);
    QVERIFY(copy->isVisible());
    copy->click();
    const auto invited = librarySettingsFromConnectionCode(QGuiApplication::clipboard()->text(),
                                                            QStringLiteral("Other computer"));
    QVERIFY(invited);
    QCOMPARE(invited->port, applied.hostedLibrary.port);
    QCOMPARE(invited->token, applied.hostedLibrary.token);
    QCOMPARE(invited->address, status.localAddress);
    window.setHostedStatus(true);
    QVERIFY(!copy->isVisible());

    mode->setCurrent(0);
    const AppSettings disabled = window.settings();
    QVERIFY(!disabled.hostedLibrary.enabled && !disabled.hostedLibrary.share);
    QCOMPARE(disabled.hostedLibrary.token, applied.hostedLibrary.token);
    QCOMPARE(disabled.hostedLibrary.directory, applied.hostedLibrary.directory);
}

void SettingsWindowTests::verifyNewConnectionBeforeSaving()
{
    SettingsWindow window(AppSettings{});
    window.show();
    auto *mode = window.findChild<Segmented *>(QStringLiteral("libraryConnection"));
    auto *code = window.findChild<QLineEdit *>(QStringLiteral("libraryInviteCode"));
    QVERIFY(mode && code);

    LibrarySettings destination;
    destination.transport = LibraryTransport::Tailscale;
    destination.address = QStringLiteral("100.101.102.103");
    destination.port = 29102;
    destination.token = generateLibraryToken();

    int tests = 0;
    int saves = 0;
    connect(&window, &SettingsWindow::testRequested, this, [&](const LibrarySettings &library) {
        ++tests;
        QCOMPARE(library.address, destination.address);
        QCOMPARE(library.port, destination.port);
    });
    connect(&window, &SettingsWindow::saveRequested, this, [&](const AppSettings &settings, bool) {
        ++saves;
        QCOMPARE(settings.library.address, destination.address);
        QCOMPARE(settings.library.port, destination.port);
        QCOMPARE(settings.library.token, destination.token);
        window.setSaveResult({}, settings);
    });

    mode->setCurrent(2);
    auto *connectButton = buttonWithText(window, QStringLiteral("Connect"));
    QVERIFY(connectButton && connectButton->isVisible());
    QCOMPARE(code->echoMode(), QLineEdit::Password);
    code->setText(QStringLiteral("invalid"));
    connectButton->click();
    QCOMPARE(tests, 0);
    QCOMPARE(saves, 0);

    code->setText(libraryConnectionCode(destination));
    connectButton->click();
    QCOMPARE(tests, 1);
    QCOMPARE(saves, 0);
    QVERIFY(!connectButton->isEnabled());
    window.setTestResult(QStringLiteral("Library is unreachable."));
    QCOMPARE(saves, 0);
    QVERIFY(connectButton->isEnabled() && window.isVisible());

    connectButton->click();
    QCOMPARE(tests, 2);
    window.setTestResult({});
    QCOMPARE(saves, 1);
    QCOMPARE(window.result(), int(QDialog::Accepted));
}

QTEST_MAIN(SettingsWindowTests)
#include "settingswindow_tests.moc"
