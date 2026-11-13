#include "annotations.h"
#include "libraryclient.h"
#include "librarywindow.h"
#include "pngexport.h"
#include "settings.h"
#include "settingswindow.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>

#if defined(Q_OS_WIN)
#include "platform/windows_shortcuts.h"
#endif

namespace {

bool check(bool condition, const char *message)
{
    if (!condition) {
        QTextStream(stderr) << message << Qt::endl;
    }
    return condition;
}

bool annotationRendering()
{
    using namespace screenframe;

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

    bool passed = check(edited.size() == source.size(), "Export must preserve the physical pixel size.");
    passed &= check(arrow.pixelColor(18, 10) == QColor(Qt::red), "Arrow must use the selected color.");
    passed &= check(arrow.pixelColor(52, 31) == source.pixelColor(52, 31), "Undo must omit later annotations.");
    passed &= check(blurred.red() > 20 && blurred.red() < 235 && blurred.red() == blurred.blue(),
                    "Blur must average nearby pixels in the selected area.");
    passed &= check(edited.pixelColor(20, 50) == QColor(Qt::blue), "Redaction must paint opaque pixels.");
    passed &= check(edited.pixelColor(90, 67) == source.pixelColor(90, 67), "Unmarked pixels must stay intact.");
    return passed;
}

bool annotationDamage()
{
    using namespace screenframe;

    QImage source(110, 80, QImage::Format_RGB32);
    source.fill(Qt::white);
    const QVector<Annotation> annotations{
        {Tool::Pen, Qt::red, 9, {{12, 56}, {32, 8}, {45, 54}}},
        {Tool::Arrow, Qt::blue, 16, {{35, 65}, {80, 23}}},
    };

    bool passed = true;
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
        passed &= check(changedInside && !changedOutside, "Undo patches must cover the complete rendered mark.");
    }
    return passed;
}

bool pngExport()
{
    const QTemporaryDir directory;
    if (!check(directory.isValid(), "A temporary directory is required for the PNG test.")) {
        return false;
    }

    const QString path = directory.filePath(QStringLiteral("снимок.png"));
    QImage image(24, 16, QImage::Format_ARGB32);
    image.fill(QColor(34, 120, 211, 180));
    QString error;
    if (!check(savePng(image, path, &error), "PNG should save to a Unicode path.")) {
        return false;
    }

    bool passed = check(!savePng(QImage{}, path, &error) && !error.isEmpty(), "Saving an invalid image must fail.");
    const QImage reopened(path);
    passed &= check(reopened.size() == image.size() && reopened.pixelColor(7, 4) == image.pixelColor(7, 4),
                    "A failed write must leave the previous PNG intact.");

    const QImage premultiplied = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QImage expected = premultiplied.convertToFormat(QImage::Format_ARGB32);
    passed &= check(savePng(premultiplied, path, &error) && QImage(path) == expected,
                    "Exporting a premultiplied image must preserve straight-alpha pixels.");
    return passed;
}

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

bool settingsValidation()
{
    const LibrarySettings empty;
    const LibrarySettings valid = validLibrary();
    bool passed = check(!empty.isConfigured() && empty.validationError().isEmpty(),
                        "A fresh, standalone profile must be valid without a shared library.");
    passed &= check(valid.isConfigured() && valid.validationError().isEmpty(), "A complete SSH profile should be valid.");

    auto candidate = valid;
    candidate.transport = LibraryTransport::Local;
    candidate.sshHost.clear();
    candidate.port = 1;
    candidate.token = QString(256, QLatin1Char('_'));
    candidate.device = QString(48, QLatin1Char('D'));
    passed &= check(candidate.isConfigured() && candidate.validationError().isEmpty(),
                    "Loopback setup should support the valid boundary lengths and minimum port.");
    candidate.port = 65535;
    passed &= check(candidate.validationError().isEmpty(), "The maximum TCP port is valid.");
    candidate = valid;
    candidate.sshHost = QString(128, QLatin1Char('h'));
    passed &= check(candidate.validationError().isEmpty(), "A 128-character SSH host alias is valid.");

    for (const int port : {0, 65536}) {
        candidate = valid;
        candidate.port = port;
        passed &= check(!candidate.isConfigured() && !candidate.validationError().isEmpty(),
                        "An out-of-range port must be rejected.");
    }
    for (const QString &host : {QString{}, QStringLiteral("-oProxyCommand"), QStringLiteral("host name"),
                                QString(129, QLatin1Char('h'))}) {
        candidate = valid;
        candidate.sshHost = host;
        passed &= check(!candidate.isConfigured() && !candidate.validationError().isEmpty(),
                        "SSH profiles must use a safe, nonempty host alias of at most 128 characters.");
    }
    for (const QString &token : {QString(31, QLatin1Char('a')), QString(257, QLatin1Char('a')),
                                 QString(32, QLatin1Char('+'))}) {
        candidate = valid;
        candidate.token = token;
        passed &= check(!candidate.isConfigured() && !candidate.validationError().isEmpty(),
                        "Tokens must have 32–256 URL-safe ASCII characters.");
    }
    for (const QString &device : {QString{}, QStringLiteral("   "), QStringLiteral("test\nname"),
                                  QString(49, QLatin1Char('D'))}) {
        candidate = valid;
        candidate.device = device;
        passed &= check(!candidate.isConfigured() && !candidate.validationError().isEmpty(),
                        "A device label must have 1–48 printable ASCII characters.");
    }
    candidate = valid;
    candidate.autoArchive = false;
    candidate.token.clear();
    passed &= check(!candidate.isConfigured() && !candidate.validationError().isEmpty(),
                    "Turning off automatic archiving must not make a partial setup valid.");
    return passed;
}

bool settingsDestinationKey()
{
    const LibrarySettings original = validLibrary();
    const QString key = original.storageKey();
    const QRegularExpression sha256(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    bool passed = check(sha256.match(key).hasMatch(), "Destination key must be a lowercase SHA-256 value.");

    auto candidate = original;
    candidate.device = QStringLiteral("Renamed desktop");
    candidate.autoArchive = false;
    passed &= check(candidate.storageKey() == key, "A device label or archive preference cannot change the queue destination.");
    candidate = original;
    candidate.sshHost = QStringLiteral("other-library");
    passed &= check(candidate.storageKey() != key, "A different SSH host must have a separate queue.");
    candidate.sshHost = original.sshHost.toUpper();
    passed &= check(candidate.storageKey() != key, "An SSH host alias with different casing may route separately.");
    candidate = original;
    ++candidate.port;
    passed &= check(candidate.storageKey() != key, "A different remote port must have a separate queue.");
    candidate = original;
    candidate.token = QString(32, QLatin1Char('b'));
    passed &= check(candidate.storageKey() != key, "A different authorization token must have a separate queue.");
    candidate = original;
    candidate.transport = LibraryTransport::Local;
    candidate.sshHost.clear();
    passed &= check(candidate.storageKey() != key, "A local service must use a separate queue from an SSH destination.");
    return passed;
}

bool writeSettingsFixture(const QString &path, const QJsonObject &settings)
{
    QFile file(path);
    const QByteArray data = QJsonDocument(settings).toJson(QJsonDocument::Compact);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

bool settingsMigration()
{
    const QTemporaryDir directory;
    if (!check(directory.isValid(), "A private fixture directory is required for settings tests.")) {
        return false;
    }

    QString error;
    const AppSettings fresh = loadSettings(directory.path(), &error);
    bool passed = check(error.isEmpty() && !fresh.library.isConfigured() && !fresh.library.autoArchive
                            && !fresh.useSystemRegionShortcut,
                        "A new installation must be standalone with the system shortcut untouched.");

    const LibrarySettings legacyLibrary = validLibrary();
    const QString legacyPath = directory.filePath(QStringLiteral("library.json"));
    if (!check(writeSettingsFixture(legacyPath, {
                    {QStringLiteral("sshHost"), legacyLibrary.sshHost},
                    {QStringLiteral("port"), legacyLibrary.port},
                    {QStringLiteral("token"), legacyLibrary.token},
                    {QStringLiteral("device"), legacyLibrary.device},
                }), "Could not write the legacy settings fixture.")) {
        return false;
    }

    const AppSettings migrated = loadSettings(directory.path(), &error);
    passed &= check(error.isEmpty() && migrated.library.isConfigured() && migrated.library.autoArchive
                        && migrated.useSystemRegionShortcut && migrated.library.transport == LibraryTransport::Ssh
                        && migrated.library.sshHost == legacyLibrary.sshHost && migrated.library.port == legacyLibrary.port
                        && migrated.library.token == legacyLibrary.token && migrated.library.device == legacyLibrary.device,
                    "A valid legacy library must retain its destination, auto-archive and system shortcut settings.");
    passed &= check(libraryQueueDirectory(migrated.library, directory.path()) == directory.filePath(QStringLiteral("queue")),
                    "A migrated library should keep access to its existing pending screenshots.");

    auto updated = migrated;
    updated.library.sshHost = QStringLiteral("another-library");
    updated.library.port = 19745;
    updated.library.device = QStringLiteral("Renamed desktop");
    updated.library.autoArchive = false;
    updated.useSystemRegionShortcut = false;
    if (!check(saveSettings(updated, directory.path(), &error) && error.isEmpty(), "Valid new settings should save.")) {
        return false;
    }
    passed &= check(QFile::exists(legacyPath) && QFile::exists(directory.filePath(QStringLiteral("settings.json"))),
                    "Saving settings should leave the legacy settings available for migration and rollback.");
#if !defined(Q_OS_WIN)
    const auto permissions = QFileInfo(directory.filePath(QStringLiteral("settings.json"))).permissions();
    const auto exposed = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    passed &= check(!(permissions & exposed), "The settings file containing the access token must be owner-only.");
#endif

    const AppSettings reopened = loadSettings(directory.path(), &error);
    passed &= check(error.isEmpty() && reopened.library.sshHost == updated.library.sshHost
                        && reopened.library.port == updated.library.port && reopened.library.device == updated.library.device
                        && reopened.library.token == updated.library.token && !reopened.library.autoArchive
                        && !reopened.useSystemRegionShortcut,
                    "New settings must take precedence over the legacy file and round-trip with their preferences.");
    const QString expectedQueue = directory.filePath(QStringLiteral("queues/") + updated.library.storageKey());
    passed &= check(libraryQueueDirectory(reopened.library, directory.path()) == expectedQueue,
                    "A different library must use its own queue rather than adopt the legacy screenshots.");

    auto invalid = updated;
    invalid.library.autoArchive = true;
    invalid.library.port = 0;
    passed &= check(!saveSettings(invalid, directory.path(), &error) && !error.isEmpty(),
                    "Saving invalid settings must return a useful error.");
    const AppSettings retained = loadSettings(directory.path());
    passed &= check(retained.library.storageKey() == reopened.library.storageKey() && !retained.library.autoArchive,
                    "A rejected save must preserve the last valid settings.");

    const AppSettings standalone;
    passed &= check(saveSettings(standalone, directory.path(), &error) && error.isEmpty(),
                    "A user must be able to save a standalone setup.");
    const AppSettings disconnected = loadSettings(directory.path(), &error);
    passed &= check(error.isEmpty() && !disconnected.library.isConfigured() && !disconnected.library.autoArchive
                        && !disconnected.useSystemRegionShortcut
                        && libraryQueueDirectory(disconnected.library, directory.path()).isEmpty(),
                    "A cleared modern setup must not silently reactivate its legacy library.");

    const QString modernPath = directory.filePath(QStringLiteral("settings.json"));
    for (const QJsonObject &invalidFile : {QJsonObject{}, QJsonObject{{QStringLiteral("schemaVersion"), 99}}}) {
        if (!check(writeSettingsFixture(modernPath, invalidFile), "Could not write the invalid modern settings fixture.")) {
            return false;
        }
        const AppSettings invalidModern = loadSettings(directory.path(), &error);
        passed &= check(!error.isEmpty() && !invalidModern.library.isConfigured() && !invalidModern.library.autoArchive
                            && !invalidModern.useSystemRegionShortcut,
                        "An unsupported settings file must report an error without turning on legacy behavior.");
    }
    return passed;
}

bool settingsDialog()
{
    SettingsWindow window(AppSettings{});
    window.show();
    auto *connection = window.findChild<QComboBox *>(QStringLiteral("libraryConnection"));
    auto *host = window.findChild<QLineEdit *>(QStringLiteral("libraryHost"));
    auto *device = window.findChild<QLineEdit *>(QStringLiteral("libraryDevice"));
    auto *token = window.findChild<QLineEdit *>(QStringLiteral("libraryToken"));
    QPushButton *test = nullptr;
    QPushButton *save = nullptr;
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Test Connection")) {
            test = button;
        } else if (button->text() == QStringLiteral("Save Settings")) {
            save = button;
        }
    }
    if (!check(connection && host && device && token && test && save, "Settings must offer library configuration controls.")) {
        return false;
    }

    bool passed = check(!host->isVisible() && !token->isVisible() && !test->isVisible(),
                        "Standalone settings should hide connection fields.");
    connection->setCurrentText(QStringLiteral("Library on this computer"));
    passed &= check(!host->isVisible() && token->isVisible() && token->echoMode() == QLineEdit::Password,
                    "Loopback settings must hide the SSH host and mask the access token.");
    connection->setCurrentText(QStringLiteral("Library over SSH"));
    passed &= check(host->isVisible(), "SSH setup must expose the host field.");
    save->click();
    passed &= check(window.isVisible() && window.result() != QDialog::Accepted,
                    "An incomplete connection must not close settings on Save.");

    const LibrarySettings fixture = validLibrary();
    host->setText(fixture.sshHost);
    device->setText(fixture.device);
    token->setText(fixture.token);
    bool testRequested = false;
    QObject::connect(&window, &SettingsWindow::testRequested, &window, [&](const LibrarySettings &library) {
        testRequested = library.storageKey() == fixture.storageKey();
    });
    test->click();
    passed &= check(testRequested && !save->isEnabled(), "Test Connection should use the form and wait for its result.");
    window.setTestResult(QStringLiteral("Fixture connection succeeded."), true);
    passed &= check(save->isEnabled(), "Settings should be editable again after the connection test.");
    save->click();
    passed &= check(window.result() == QDialog::Accepted && window.settings().library.storageKey() == fixture.storageKey(),
                    "Valid SSH settings should save with the selected connection.");
    return passed;
}

bool standaloneLibrary()
{
    LibraryClient client(LibrarySettings{});
    LibraryWindow window(client);
    window.show();
    QPushButton *setup = nullptr;
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Set up library")) {
            setup = button;
            break;
        }
    }
    if (!check(setup && setup->isVisible(), "An unconfigured Library must offer setup from the empty state.")) {
        return false;
    }
    bool requested = false;
    QObject::connect(&window, &LibraryWindow::settingsRequested, &window, [&] { requested = true; });
    setup->click();
    return check(requested, "Set up library must open the app's settings flow.");
}

bool libraryRejectsStaleSettings()
{
    const QTemporaryDir directory;
    if (!check(directory.isValid(), "A private fixture directory is required for Library privacy tests.")) {
        return false;
    }

    AppSettings initial;
    initial.library = validLibrary();
    if (!check(saveSettings(initial, directory.path()), "Could not save the initial Library fixture.")) {
        return false;
    }
    LibraryClient existing(initial.library, directory.path());
    QImage image(16, 12, QImage::Format_RGB32);
    image.fill(Qt::blue);
    const QDateTime capturedAt = QDateTime::fromMSecsSinceEpoch(1'700'000'000'000, Qt::UTC);
    const auto queue = QDir(libraryQueueDirectory(initial.library, directory.path()));
    const auto entries = [&queue] { return queue.entryList(QDir::Dirs | QDir::NoDotAndDotDot); };
    const auto enqueue = [&] {
        existing.enqueue(image, QStringLiteral("full"), capturedAt);
        return existing.flushPendingWrites();
    };

    bool passed = check(enqueue() && entries().size() == 1,
                        "An active Library may queue a capture when its settings still match.");
    const QStringList originalEntries = entries();

    auto paused = initial;
    paused.library.autoArchive = false;
    if (!check(saveSettings(paused, directory.path()), "Could not pause archiving in the Library fixture.")) {
        return false;
    }
    passed &= check(enqueue() && entries() == originalEntries,
                    "An older Library instance must honor the persisted archive opt-out.");

    auto other = initial;
    other.library.sshHost = QStringLiteral("another-library");
    if (!check(saveSettings(other, directory.path()), "Could not replace the Library destination fixture.")) {
        return false;
    }
    passed &= check(enqueue() && entries() == originalEntries
                        && !QDir(libraryQueueDirectory(other.library, directory.path())).exists(),
                    "An older Library instance must not queue a new capture after the destination changes.");
    return passed;
}

#if defined(Q_OS_WIN)
bool windowsRegionKey()
{
    using Decision = WindowsRegionKeyState::Decision;
    WindowsRegionKeyState key;
    bool passed = check(key.onS(true, true) == Decision::Capture, "Win+S must trigger region capture.");
    passed &= check(key.onS(true, true) == Decision::Suppress, "Holding Win+S must suppress repeats.");
    passed &= check(key.onS(true, false) == Decision::Suppress && key.onS(false, false) == Decision::Suppress,
                    "Releasing Win first must keep the matching S repeat and key-up suppressed.");
    passed &= check(key.onS(true, false) == Decision::Pass && key.onS(true, true) == Decision::Pass
                    && key.onS(false, true) == Decision::Pass,
                    "A key held before Win must pass through until released.");
    passed &= check(key.onS(true, true) == Decision::Capture && key.onS(false, true) == Decision::Suppress,
                    "A fresh Win+S press must trigger again when Win remains held.");

    WindowsRegionKeyState heldAtStart(true);
    passed &= check(heldAtStart.onS(true, true) == Decision::Pass && heldAtStart.onS(false, true) == Decision::Pass,
                    "A key held while starting the hook must finish without interception.");
    return passed;
}
#endif

} // namespace

int main(int argc, char **argv)
{
    const QApplication application(argc, argv);
    const bool annotationsPassed = annotationRendering();
    const bool damagePassed = annotationDamage();
    const bool exportPassed = pngExport();
    const bool settingsPassed = settingsValidation();
    const bool destinationPassed = settingsDestinationKey();
    const bool migrationPassed = settingsMigration();
    const bool dialogPassed = settingsDialog();
    const bool libraryPassed = standaloneLibrary();
    const bool libraryPrivacyPassed = libraryRejectsStaleSettings();
#if defined(Q_OS_WIN)
    const bool shortcutPassed = windowsRegionKey();
#else
    const bool shortcutPassed = true;
#endif
    return annotationsPassed && damagePassed && exportPassed && settingsPassed && destinationPassed
        && migrationPassed && dialogPassed && libraryPassed && libraryPrivacyPassed && shortcutPassed ? 0 : 1;
}
