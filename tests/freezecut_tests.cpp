#include "annotations.h"
#include "controls.h"
#include "libraryclient.h"
#include "librarywindow.h"
#include "pngexport.h"
#include "regionselector.h"
#include "settings.h"
#include "settingswindow.h"
#include "welcomewindow.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <memory>

#if defined(Q_OS_WIN)
#include "platform/windows_shortcuts.h"
#elif defined(Q_OS_LINUX)
#include "application.h"

#include <QClipboard>
#endif

namespace {

LibrarySettings validLibrary()
{
    LibrarySettings library;
    library.transport = LibraryTransport::Ssh;
    library.sshHost = QStringLiteral("example-library");
    library.token = QString(32, QLatin1Char('a'));
    library.device = QStringLiteral("Test desktop");
    library.autoArchive = true;
    return library;
}

bool writeJson(const QString &path, const QJsonObject &object)
{
    QFile file(path);
    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QPushButton *buttonWithText(QWidget &window, const QString &text)
{
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

} // namespace

class FreezeCutTests final : public QObject {
    Q_OBJECT

private slots:
    void annotationRendering();
    void annotationDamage();
    void pngExport();
    void regionSelectionHandsOffBeforeDismiss();
    void settingsValidation();
    void settingsDestinationKey();
    void settingsRoundTrip();
    void welcomeUsesLocalProfile();
    void welcomeNavigation();
#ifndef FREEZECUT_WINDOWS_STORE
    void settingsDialog();
#endif
    void standaloneLibrary();
    void libraryRejectsStaleSettings();
#if !defined(Q_OS_WIN) && !defined(FREEZECUT_MAC_APP_STORE)
    void settingsMoveFromScreenFrame();
#endif
#if defined(Q_OS_WIN)
    void windowsRegionKey();
#elif defined(Q_OS_LINUX)
    void captureCopiesImageToClipboard();
#endif
};

void FreezeCutTests::welcomeUsesLocalProfile()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const QDir fresh(root.filePath(QStringLiteral("fresh")));
    QVERIFY(QDir().mkpath(fresh.filePath(QStringLiteral("history"))));
    QVERIFY(QDir().mkpath(fresh.filePath(QStringLiteral("library"))));
    QVERIFY(!welcomeCompleted(fresh.path()));
    QVERIFY(markWelcomeCompleted(fresh.path()));
    QVERIFY(welcomeCompleted(fresh.path()));
    QVERIFY(!QFileInfo::exists(fresh.filePath(QStringLiteral("settings.json"))));

    const QDir configured(root.filePath(QStringLiteral("configured")));
    QVERIFY(QDir().mkpath(configured.path()));
    QFile settings(configured.filePath(QStringLiteral("settings.json")));
    QVERIFY(settings.open(QIODevice::WriteOnly));
    QCOMPARE(settings.write("unreadable", 10), qint64(10));
    settings.close();
    QVERIFY(welcomeCompleted(configured.path()));

    const QString historyDirectory = root.filePath(QStringLiteral("existing"));
    LibraryClient history(historyDirectory, {});
    QVERIFY(!welcomeCompleted(historyDirectory));
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(!history.enqueue(image, QStringLiteral("region"), QDateTime::currentDateTimeUtc()).isEmpty());
    QVERIFY(history.flushPendingWrites());
    QVERIFY(welcomeCompleted(historyDirectory));
}

void FreezeCutTests::welcomeNavigation()
{
    WelcomeWindow welcome;
    welcome.show();
    QSignalSpy finished(&welcome, &QDialog::finished);
    auto *next = buttonWithText(welcome, QStringLiteral("Continue"));
    auto *back = buttonWithText(welcome, QStringLiteral("Back"));
    auto *open = buttonWithText(welcome, QStringLiteral("Open FreezeCut"));
    QVERIFY(next && back && open);
    QVERIFY(!back->isVisible());

    next->click();
    QCOMPARE(next->text(), QStringLiteral("Try a capture"));
    QCOMPARE(next->accessibleName(), next->text());
    QVERIFY(back->isVisible());
    back->click();
    QCOMPARE(next->text(), QStringLiteral("Continue"));
    QCOMPARE(next->accessibleName(), next->text());
    QVERIFY(finished.isEmpty());

    next->click();
    next->click();
    QCOMPARE(welcome.result(), int(WelcomeWindow::TryCapture));
    QCOMPARE(finished.count(), 1);

    welcome.show();
    open->click();
    QCOMPARE(welcome.result(), int(WelcomeWindow::OpenApp));
    QCOMPARE(finished.count(), 2);
}

void FreezeCutTests::annotationRendering()
{
    QImage source(96, 72, QImage::Format_RGB32);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x) {
            source.setPixelColor(x, y, (x + y) % 2 ? Qt::white : Qt::black);
        }
    }
    source.setDevicePixelRatio(2);

    const QVector<Annotation> annotations{
        {Tool::Arrow, Qt::red, 5, {{8, 10}, {32, 10}}},
        {Tool::Blur, Qt::black, 9, {{40, 20}, {65, 46}}},
        {Tool::Redact, Qt::blue, 5, {{12, 40}, {30, 61}}},
    };
    const QImage arrow = renderAnnotations(source, annotations, 1);
    const QImage edited = renderAnnotations(source, annotations, annotations.size());
    const QColor blurred = edited.pixelColor(52, 31);

    QVERIFY2(edited.size() == source.size(), "Export must preserve the physical pixel size.");
    QVERIFY2(arrow.pixelColor(18, 10) == QColor(Qt::red), "Arrow must use the selected color.");
    QVERIFY2(arrow.pixelColor(52, 31) == source.pixelColor(52, 31), "Undo must omit later annotations.");
    QVERIFY2(blurred.red() > 20 && blurred.red() < 235 && blurred.red() == blurred.blue(),
             "Blur must average nearby pixels in the selected area.");
    QVERIFY2(edited.pixelColor(20, 50) == QColor(Qt::blue), "Redaction must paint opaque pixels.");
    QVERIFY2(edited.pixelColor(90, 67) == source.pixelColor(90, 67), "Unmarked pixels must stay intact.");
}

void FreezeCutTests::annotationDamage()
{
    QImage source(110, 80, QImage::Format_RGB32);
    source.fill(Qt::white);
    const QVector<Annotation> annotations{
        {Tool::Pen, Qt::red, 9, {{12, 56}, {32, 8}, {45, 54}}},
        {Tool::Arrow, Qt::blue, 16, {{35, 65}, {80, 23}}},
    };

    for (const auto &annotation : annotations) {
        const QImage rendered = renderAnnotations(source, {annotation}, 1);
        const QRect damage = annotation.affectedPixels();
        bool changedInside = false;
        bool changedOutside = false;
        for (int y = 0; y < source.height(); ++y) {
            for (int x = 0; x < source.width(); ++x) {
                if (source.pixel(x, y) != rendered.pixel(x, y)) {
                    (damage.contains(x, y) ? changedInside : changedOutside) = true;
                }
            }
        }
        QVERIFY2(changedInside && !changedOutside, "Undo patches must cover the complete rendered mark.");
    }
}

void FreezeCutTests::pngExport()
{
    const QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path = directory.filePath(QStringLiteral("снимок.png"));
    QImage image(24, 16, QImage::Format_ARGB32);
    image.fill(QColor(34, 120, 211, 180));
    QString error;
    QVERIFY2(savePng(image, path, &error), "PNG should save to a Unicode path.");

    QVERIFY2(!savePng(QImage{}, path, &error) && !error.isEmpty(), "Saving an invalid image must fail.");
    const QImage reopened(path);
    QVERIFY2(reopened.size() == image.size() && reopened.pixelColor(7, 4) == image.pixelColor(7, 4),
             "A failed write must leave the previous PNG intact.");

    const QImage premultiplied = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage expected = premultiplied.convertToFormat(QImage::Format_ARGB32);
    QVERIFY2(savePng(premultiplied, path, &error) && QImage(path) == expected,
             "Exporting a premultiplied image must preserve straight-alpha pixels.");
}

void FreezeCutTests::regionSelectionHandsOffBeforeDismiss()
{
    const bool quitOnClose = QApplication::quitOnLastWindowClosed();
    QApplication::setQuitOnLastWindowClosed(false);
    const auto restore = qScopeGuard([quitOnClose] { QApplication::setQuitOnLastWindowClosed(quitOnClose); });

    struct Selection {
        QPointer<QWidget> overlay;
        QImage image;
        bool completed = false;
        bool visibleDuringHandoff = false;
        bool hiddenAfterDismiss = false;
    };
    const auto selection = std::make_shared<Selection>();
    QImage frame(160, 100, QImage::Format_RGB32);
    frame.fill(Qt::blue);
    RegionSelector::start({CaptureFrame{frame, nullptr}}, [selection](QImage image, const auto &dismiss) {
        selection->visibleDuringHandoff = selection->overlay && selection->overlay->isVisible();
        dismiss();
        selection->hiddenAfterDismiss = !selection->overlay || !selection->overlay->isVisible();
        selection->image = std::move(image);
        selection->completed = true;
    });
    for (auto *window : QApplication::topLevelWidgets()) {
        if (window->isVisible() && window->windowTitle() == QStringLiteral("FreezeCut — Select region")) {
            selection->overlay = window;
            break;
        }
    }
    const auto &overlay = selection->overlay;
    QVERIFY(overlay);
    const auto close = qScopeGuard([&overlay] {
        if (overlay && overlay->isVisible())
            overlay->close();
    });
    overlay->resize(800, 500);
    const QPoint center = overlay->rect().center();
    const QPoint start = center - QPoint(36, 24);
    const QPoint end = center + QPoint(36, 24);
    QMouseEvent press(QEvent::MouseButtonPress, start, overlay->mapToGlobal(start),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, end, overlay->mapToGlobal(end),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(overlay, &press);
    QCoreApplication::sendEvent(overlay, &release);

    QTRY_VERIFY_WITH_TIMEOUT(selection->completed, 1000);
    QVERIFY(selection->visibleDuringHandoff);
    QVERIFY(selection->hiddenAfterDismiss);
    QVERIFY(!selection->image.isNull());
    QCOMPARE(selection->image.pixelColor(0, 0), QColor(Qt::blue));
}

void FreezeCutTests::settingsValidation()
{
    const LibrarySettings empty;
    const LibrarySettings valid = validLibrary();
    QVERIFY2(!empty.isConfigured() && empty.validationError().isEmpty(),
             "A fresh, standalone profile must be valid without a shared library.");
    QVERIFY2(valid.isConfigured() && valid.validationError().isEmpty(), "A complete SSH profile should be valid.");

    auto candidate = valid;
    candidate.transport = LibraryTransport::Local;
    candidate.sshHost.clear();
    candidate.port = 1;
    candidate.token = QString(256, QLatin1Char('_'));
    candidate.device = QString(48, QLatin1Char('D'));
    QVERIFY2(candidate.isConfigured() && candidate.validationError().isEmpty(),
             "Loopback setup should support the valid boundary lengths and minimum port.");
    candidate.port = 65535;
    QVERIFY2(candidate.validationError().isEmpty(), "The maximum TCP port is valid.");
    candidate = valid;
    candidate.sshHost = QString(128, QLatin1Char('h'));
    QVERIFY2(candidate.validationError().isEmpty(), "A 128-character SSH host alias is valid.");

    for (const int port : {0, 65536}) {
        candidate = valid;
        candidate.port = port;
        QVERIFY2(!candidate.isConfigured() && !candidate.validationError().isEmpty(),
                 "An out-of-range port must be rejected.");
    }
    for (const QString &host : {QString{}, QStringLiteral("-oProxyCommand"), QStringLiteral("host name"),
                                QString(129, QLatin1Char('h'))}) {
        candidate = valid;
        candidate.sshHost = host;
        QVERIFY2(!candidate.isConfigured() && !candidate.validationError().isEmpty(),
                 "SSH profiles must use a safe, nonempty host alias of at most 128 characters.");
    }
    for (const QString &token : {QString(31, QLatin1Char('a')), QString(257, QLatin1Char('a')),
                                 QString(32, QLatin1Char('+'))}) {
        candidate = valid;
        candidate.token = token;
        QVERIFY2(!candidate.isConfigured() && !candidate.validationError().isEmpty(),
                 "Tokens must have 32–256 URL-safe ASCII characters.");
    }
    for (const QString &device : {QString{}, QStringLiteral("   "), QStringLiteral("test\nname"),
                                  QString(49, QLatin1Char('D'))}) {
        candidate = valid;
        candidate.device = device;
        QVERIFY2(!candidate.isConfigured() && !candidate.validationError().isEmpty(),
                 "A device label must have 1–48 printable ASCII characters.");
    }
    candidate = valid;
    candidate.autoArchive = false;
    candidate.token.clear();
    QVERIFY2(!candidate.isConfigured() && !candidate.validationError().isEmpty(),
             "Turning off automatic archiving must not make a partial setup valid.");
}

void FreezeCutTests::settingsDestinationKey()
{
    const LibrarySettings original = validLibrary();
    const QString key = original.storageKey();
    const QRegularExpression sha256(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    QVERIFY2(sha256.match(key).hasMatch(), "Destination key must be a lowercase SHA-256 value.");

    auto candidate = original;
    candidate.device = QStringLiteral("Renamed desktop");
    candidate.autoArchive = false;
    QVERIFY2(candidate.storageKey() == key, "A device label or archive preference cannot change the queue destination.");
    candidate = original;
    candidate.sshHost = QStringLiteral("other-library");
    QVERIFY2(candidate.storageKey() != key, "A different SSH host must have a separate queue.");
    candidate.sshHost = original.sshHost.toUpper();
    QVERIFY2(candidate.storageKey() != key, "An SSH host alias with different casing may route separately.");
    candidate = original;
    ++candidate.port;
    QVERIFY2(candidate.storageKey() != key, "A different remote port must have a separate queue.");
    candidate = original;
    candidate.token = QString(32, QLatin1Char('b'));
    QVERIFY2(candidate.storageKey() != key, "A different authorization token must have a separate queue.");
    candidate = original;
    candidate.transport = LibraryTransport::Local;
    candidate.sshHost.clear();
    QVERIFY2(candidate.storageKey() != key, "A local service must use a separate queue from an SSH destination.");
}

void FreezeCutTests::settingsRoundTrip()
{
    const QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QString error;
    const AppSettings fresh = loadSettings(directory.path(), &error);
    QVERIFY2(error.isEmpty() && !fresh.library.isConfigured() && !fresh.library.autoArchive
                 && !fresh.useSystemRegionShortcut,
             "A new installation must be standalone with the system shortcut untouched.");

    AppSettings saved;
    saved.library = validLibrary();
    saved.library.port = 19745;
    saved.useSystemRegionShortcut = true;
    QVERIFY2(saveSettings(saved, directory.path(), &error) && error.isEmpty(), "Valid settings should save.");
#if !defined(Q_OS_WIN)
    const auto permissions = QFileInfo(directory.filePath(QStringLiteral("settings.json"))).permissions();
    const auto exposed = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    QVERIFY2(!(permissions & exposed), "The settings file containing the access token must be owner-only.");
#endif

    const AppSettings reopened = loadSettings(directory.path(), &error);
    QVERIFY2(error.isEmpty() && reopened == saved, "Settings must round-trip unchanged.");
    const QString expectedQueue = directory.filePath(QStringLiteral("queues/") + saved.library.storageKey());
    QVERIFY2(libraryQueueDirectory(reopened.library, directory.path()) == expectedQueue,
             "Each library destination must use its own queue.");

    auto invalid = saved;
    invalid.library.port = 0;
    QVERIFY2(!saveSettings(invalid, directory.path(), &error) && !error.isEmpty(),
             "Saving invalid settings must return a useful error.");
    QVERIFY2(loadSettings(directory.path()) == saved, "A rejected save must preserve the last valid settings.");

    QVERIFY2(saveSettings(AppSettings{}, directory.path(), &error) && error.isEmpty(),
             "A user must be able to save a standalone setup.");
    const AppSettings disconnected = loadSettings(directory.path(), &error);
    QVERIFY2(error.isEmpty() && disconnected == AppSettings{}
                 && libraryQueueDirectory(disconnected.library, directory.path()).isEmpty(),
             "A cleared setup must load as standalone.");

    const QString path = directory.filePath(QStringLiteral("settings.json"));
    for (const QJsonObject &invalidFile : {QJsonObject{}, QJsonObject{{QStringLiteral("schemaVersion"), 99}}}) {
        QVERIFY(writeJson(path, invalidFile));
        const AppSettings unsupported = loadSettings(directory.path(), &error);
        QVERIFY2(!error.isEmpty() && unsupported == AppSettings{},
                 "An unsupported settings file must report an error and load as standalone.");
    }
}

#ifndef FREEZECUT_WINDOWS_STORE
void FreezeCutTests::settingsDialog()
{
    SettingsWindow window(AppSettings{});
    window.show();
    auto *connection = window.findChild<Segmented *>(QStringLiteral("libraryConnection"));
    auto *host = window.findChild<QLineEdit *>(QStringLiteral("libraryHost"));
    auto *device = window.findChild<QLineEdit *>(QStringLiteral("libraryDevice"));
    auto *token = window.findChild<QLineEdit *>(QStringLiteral("libraryToken"));
    auto *test = buttonWithText(window, QStringLiteral("Test connection"));
    auto *save = buttonWithText(window, QStringLiteral("Save"));
    QVERIFY2(connection && host && device && token && test && save, "Settings must offer library configuration controls.");

    QVERIFY2(!host->isVisible() && !token->isVisible() && !test->isVisible(),
             "Standalone settings should hide connection fields.");
    connection->setCurrent(3);
    auto *transport = window.findChild<Segmented *>(QStringLiteral("libraryManualTransport"));
    QVERIFY(transport);
    transport->setCurrent(0);
    QVERIFY2(!host->isVisible() && token->isVisible() && token->echoMode() == QLineEdit::Password,
             "Loopback settings must hide the SSH host and mask the access token.");
    transport->setCurrent(1);
#ifdef FREEZECUT_MAC_APP_STORE
    QVERIFY2(!transport->isVisible() && !host->isVisible(), "The Store build must hide SSH setup.");
#else
    QVERIFY2(host->isVisible(), "SSH setup must expose the host field.");
#endif
    save->click();
    QVERIFY2(window.isVisible() && window.result() != QDialog::Accepted,
             "An incomplete connection must not close settings on Save.");

    LibrarySettings fixture = validLibrary();
#ifdef FREEZECUT_MAC_APP_STORE
    fixture.transport = LibraryTransport::Local;
    fixture.sshHost.clear();
#endif
    host->setText(fixture.sshHost);
    device->setText(fixture.device);
    token->setText(fixture.token);
    bool testRequested = false;
    connect(&window, &SettingsWindow::testRequested, &window, [&](const LibrarySettings &library) {
        testRequested = library.storageKey() == fixture.storageKey();
    });
    test->click();
    QVERIFY2(testRequested && !save->isEnabled(), "Test Connection should use the form and wait for its result.");
    window.setTestResult({});
    QVERIFY2(save->isEnabled(), "Settings should be editable again after the connection test.");
    connect(&window, &SettingsWindow::saveRequested, &window, [&](const AppSettings &settings, bool) {
        window.setSaveResult({}, settings);
    });
    save->click();
    QVERIFY2(window.result() == QDialog::Accepted && window.settings().library.storageKey() == fixture.storageKey(),
             "Valid library settings should save with the selected connection.");
}
#endif

void FreezeCutTests::standaloneLibrary()
{
    const QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LibraryClient client(directory.path(), {});
    LibraryWindow window(client);
    window.show();
    const auto hasEmptyState = [&window] {
        for (const auto *label : window.findChildren<QLabel *>(QStringLiteral("emptyTitle"))) {
            if (label->isVisible() && label->text() == QStringLiteral("No screenshots yet"))
                return true;
        }
        return false;
    };
    QTRY_VERIFY(hasEmptyState());
    QPushButton *settings = nullptr;
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->accessibleName() == QStringLiteral("Settings")) {
            settings = button;
            break;
        }
    }
    QVERIFY(settings && settings->isVisible());
    bool requested = false;
    connect(&window, &LibraryWindow::settingsRequested, &window, [&] { requested = true; });
    settings->click();
    QVERIFY(requested);

    auto *grid = window.findChild<QListView *>();
    QVERIFY(grid);
    QImage image(16, 12, QImage::Format_RGB32);
    image.fill(Qt::blue);
    QVERIFY(!client.enqueue(image, QStringLiteral("full"), QDateTime::currentDateTimeUtc()).isEmpty());
    QTRY_COMPARE(grid->model()->rowCount(), 1);
    QVERIFY(grid->isVisible());
}

void FreezeCutTests::libraryRejectsStaleSettings()
{
    const QTemporaryDir directory;
    QVERIFY(directory.isValid());

    AppSettings initial;
    initial.library = validLibrary();
    QVERIFY(saveSettings(initial, directory.path()));
    LibraryClient existing(initial.library, directory.path());
    QImage image(16, 12, QImage::Format_RGB32);
    image.fill(Qt::blue);
    const QDateTime capturedAt = QDateTime::fromMSecsSinceEpoch(1'700'000'000'000, QTimeZone::utc());
    const auto queue = QDir(libraryQueueDirectory(initial.library, directory.path()));
    const auto entries = [&queue] { return queue.entryList(QDir::Dirs | QDir::NoDotAndDotDot); };
    const auto enqueue = [&] {
        existing.enqueue(image, QStringLiteral("full"), capturedAt);
        return existing.flushPendingWrites();
    };

    QVERIFY2(enqueue() && entries().size() == 1, "An active Library may queue a capture when its settings still match.");
    const QStringList originalEntries = entries();

    auto paused = initial;
    paused.library.autoArchive = false;
    QVERIFY(saveSettings(paused, directory.path()));
    QVERIFY2(enqueue() && entries() == originalEntries, "An older Library instance must honor the persisted archive opt-out.");

    auto other = initial;
    other.library.sshHost = QStringLiteral("another-library");
    QVERIFY(saveSettings(other, directory.path()));
    QVERIFY2(enqueue() && entries() == originalEntries
                 && !QDir(libraryQueueDirectory(other.library, directory.path())).exists(),
             "An older Library instance must not queue a new capture after the destination changes.");
}

#if !defined(Q_OS_WIN) && !defined(FREEZECUT_MAC_APP_STORE)
void FreezeCutTests::settingsMoveFromScreenFrame()
{
    const QTemporaryDir home;
    QVERIFY(home.isValid());
    const QByteArray previousHome = qgetenv("HOME");
    qputenv("HOME", QFile::encodeName(home.path()));
    const auto restoreHome = qScopeGuard([&] { qputenv("HOME", previousHome); });

    const QDir root(home.path());
    const QString legacy = root.filePath(QStringLiteral(".screenframe"));
    AppSettings saved;
    saved.hostedLibrary.enabled = true;
    saved.hostedLibrary.directory = QDir(legacy).filePath(QStringLiteral("library"));
    saved.hostedLibrary.token = QString(32, QLatin1Char('b'));
    QString error;
    QVERIFY2(saveSettings(saved, legacy, &error), qPrintable(error));
    QVERIFY(QDir(legacy).mkpath(QStringLiteral("queues/key")) && QDir(legacy).mkpath(QStringLiteral("library")));

    const QString directory = defaultSettingsDirectory();
    QVERIFY2(directory == root.filePath(QStringLiteral(".frameline")), "Settings must live in the existing .frameline profile folder after the move.");
    QVERIFY2(QFileInfo(QDir(directory).filePath(QStringLiteral("queues/key"))).isDir()
                 && QFileInfo(QDir(directory).filePath(QStringLiteral("library"))).isDir() && !QFileInfo::exists(legacy),
             "Moving from ScreenFrame must carry settings, queued uploads and the library along.");
    const AppSettings loaded = loadSettings(directory, &error);
    QVERIFY2(error.isEmpty() && loaded.hostedLibrary.directory == QDir(directory).filePath(QStringLiteral("library")),
             "A library kept in the old settings folder must be found in the new one.");
    QVERIFY2(defaultSettingsDirectory() == directory, "The move must happen only once.");
}
#endif

#if defined(Q_OS_WIN)
void FreezeCutTests::windowsRegionKey()
{
    using Decision = WindowsRegionKeyState::Decision;
    WindowsRegionKeyState key;
    QVERIFY2(key.onS(true, true) == Decision::Capture, "Win+S must trigger region capture.");
    QVERIFY2(key.onS(true, true) == Decision::Suppress, "Holding Win+S must suppress repeats.");
    QVERIFY2(key.onS(true, false) == Decision::Suppress && key.onS(false, false) == Decision::Suppress,
             "Releasing Win first must keep the matching S repeat and key-up suppressed.");
    QVERIFY2(key.onS(true, false) == Decision::Pass && key.onS(true, true) == Decision::Pass
                 && key.onS(false, true) == Decision::Pass,
             "A key held before Win must pass through until released.");
    QVERIFY2(key.onS(true, true) == Decision::Capture && key.onS(false, true) == Decision::Suppress,
             "A fresh Win+S press must trigger again when Win remains held.");

    WindowsRegionKeyState heldAtStart(true);
    QVERIFY2(heldAtStart.onS(true, true) == Decision::Pass && heldAtStart.onS(false, true) == Decision::Pass,
             "A key held while starting the hook must finish without interception.");
}
#elif defined(Q_OS_LINUX)
void FreezeCutTests::captureCopiesImageToClipboard()
{
    if (QApplication::platformName() != QStringLiteral("offscreen") || ScreenCapture::usesScreenshotPortal())
        QSKIP("This test requires the offscreen platform without the Wayland screenshot portal.");

    QTemporaryDir home;
    QVERIFY(home.isValid());
    const QByteArray previousHome = qgetenv("HOME");
    const bool quitOnLastWindow = QApplication::quitOnLastWindowClosed();
    const auto restore = qScopeGuard([&] {
        if (previousHome.isNull())
            qunsetenv("HOME");
        else
            qputenv("HOME", previousHome);
        QApplication::setQuitOnLastWindowClosed(quitOnLastWindow);
    });
    QVERIFY(qputenv("HOME", QFile::encodeName(home.path())));
    QCOMPARE(QDir::homePath(), home.path());

    QWidget sample;
    sample.setStyleSheet(QStringLiteral("background: #246798;"));
    sample.showFullScreen();

    const QDir history(home.filePath(QStringLiteral(".frameline/history")));
    const auto entries = [&history] { return history.entryList(QDir::Dirs | QDir::NoDotAndDotDot); };
    for (bool enabled : {true, false}) {
        const QStringList before = entries();
        if (!enabled) {
            AppSettings settings;
            settings.autoCopyScreenshot = false;
            settings.showEditorAfterCapture = false;
            QString error;
            QVERIFY2(saveSettings(settings, defaultSettingsDirectory(), &error), qPrintable(error));
        }
        auto *clipboard = QApplication::clipboard();
        const QString original = QStringLiteral("Previous clipboard content");
        clipboard->setText(original);

        const QString path = home.filePath(enabled ? QStringLiteral("enabled.png") : QStringLiteral("disabled.png"));
        FreezeCutApplication application({LaunchOptions::Mode::Full, path});
        application.start();
        QTRY_VERIFY(QFileInfo::exists(path));
        bool editorVisible = false;
        for (const auto *window : QApplication::topLevelWidgets()) {
            if (qobject_cast<const EditorWindow *>(window) && window->isVisible()) {
                editorVisible = true;
                break;
            }
        }
        QCOMPARE(editorVisible, enabled);
        const QImage captured(path);
        QVERIFY(!captured.isNull());
        if (enabled)
            QCOMPARE(clipboard->image().convertToFormat(QImage::Format_RGB32),
                     captured.convertToFormat(QImage::Format_RGB32));
        else
            QCOMPARE(clipboard->text(), original);

        QTRY_COMPARE(entries().size(), before.size() + 1);
        auto added = entries();
        for (const QString &id : before)
            added.removeOne(id);
        QCOMPARE(added.size(), 1);
        const QImage archived(history.filePath(added.constFirst() + QStringLiteral("/image.png")));
        QCOMPARE(archived.convertToFormat(QImage::Format_RGB32), captured.convertToFormat(QImage::Format_RGB32));
    }
}
#endif

QTEST_MAIN(FreezeCutTests)
#include "freezecut_tests.moc"
