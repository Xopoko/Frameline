// Developer tool: renders FreezeCut's windows to PNG files for visual review and records the
// frames of the README demo.
// Usage: freezecut_shots <output-directory> [<library-port> <library-token> | --demo-welcome | --region | --demo-editor | --demo-region]
#include "editor.h"
#include "regionselector.h"
#include "libraryclient.h"
#include "librarywindow.h"
#include "settings.h"
#include "settingswindow.h"
#include "controls.h"
#include "tailscale.h"
#include "welcomewindow.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScreen>
#include <QTemporaryDir>
#include <QTest>

namespace {

QString outputDirectory;
bool saveFailed = false;

void settle(int ms = 350)
{
    QTest::qWait(ms);
}

QPixmap save(QWidget &window, const QString &path)
{
    const QPixmap image = window.grab();
    if (!image.save(path)) {
        qWarning("Could not write %s", qPrintable(path));
        saveFailed = true;
    }
    return image;
}

void shoot(QWidget &window, const QString &name)
{
    settle();
    save(window, QDir(outputDirectory).filePath(name + QStringLiteral(".png")));
}

void present(QWidget &window, QSize size)
{
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(size);
    // Nothing places a window that never reaches the screen. Left in the screen's corner it lies under
    // the menu bar and a MacBook's camera housing, where Qt insets the contents of a window.
    window.move(window.screen()->availableGeometry().topLeft() + QPoint(40, 40));
    window.show();
    settle(200);
}

// A stand-in for a real capture: a desktop with an app window, so marks have something to point at.
QImage sampleCapture(QSize size = QSize(2560, 1600))
{
    QImage image(size, QImage::Format_RGB32);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient sky(0, 0, size.width(), size.height());
    sky.setColorAt(0, QColor("#f3d9c4"));
    sky.setColorAt(0.5, QColor("#d98f6e"));
    sky.setColorAt(1, QColor("#3d3558"));
    p.fillRect(image.rect(), sky);
    const qreal s = size.width() / 2560.0;
    p.scale(s, s);
    p.fillRect(QRectF(0, 0, 2560, 50), QColor(255, 255, 255, 150));
    QFont font = p.font();
    font.setPixelSize(24);
    p.setFont(font);
    p.setPen(QColor("#222"));
    p.drawText(QRectF(28, 0, 800, 50), Qt::AlignVCenter, QStringLiteral("Atlas    File    Edit    View    Window    Help"));
    const QRectF window(330, 170, 1900, 1220);
    QPainterPath shape;
    shape.addRoundedRect(window, 22, 22);
    p.fillPath(shape.translated(0, 26), QColor(0, 0, 0, 60));
    p.fillPath(shape, QColor("#fbfaf8"));
    p.setClipPath(shape);
    p.fillRect(QRectF(window.left(), window.top(), 420, window.height()), QColor("#f0ede8"));
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 3; ++i) {
        p.setBrush(QColor(i == 0 ? "#ee6a5f" : i == 1 ? "#f5bd4f" : "#61c454"));
        p.drawEllipse(QPointF(window.left() + 40 + i * 34, window.top() + 40), 11, 11);
    }
    font.setPixelSize(26);
    p.setFont(font);
    const QStringList rows{QStringLiteral("Overview"), QStringLiteral("Deployments"), QStringLiteral("Incidents"),
                           QStringLiteral("Billing"), QStringLiteral("API keys"), QStringLiteral("Team")};
    for (int i = 0; i < rows.size(); ++i) {
        const QRectF row(window.left() + 24, window.top() + 110 + i * 62, 372, 52);
        if (i == 4) {
            p.setBrush(QColor("#dedad2"));
            p.drawRoundedRect(row, 10, 10);
        }
        p.setPen(QColor("#33312e"));
        p.drawText(row.adjusted(20, 0, 0, 0), Qt::AlignVCenter, rows[i]);
        p.setPen(Qt::NoPen);
    }
    font.setPixelSize(46);
    font.setWeight(QFont::DemiBold);
    p.setFont(font);
    p.setPen(QColor("#1d1c1a"));
    p.drawText(QPointF(window.left() + 480, window.top() + 130), QStringLiteral("API keys"));
    font.setPixelSize(26);
    font.setWeight(QFont::Normal);
    p.setFont(font);
    const QStringList names{QStringLiteral("Production"), QStringLiteral("Staging"), QStringLiteral("CI runner"),
                            QStringLiteral("Local development")};
    const QStringList keys{QStringLiteral("sk_live_9f2c41d07ab36e58"), QStringLiteral("sk_test_51b77c0e93aa1f04"),
                           QStringLiteral("sk_test_c02e6d1184f9b7a3"), QStringLiteral("sk_test_7d40a9e2c6315bf8")};
    for (int i = 0; i < names.size(); ++i) {
        const QRectF row(window.left() + 480, window.top() + 200 + i * 118, 1360, 96);
        p.setPen(QPen(QColor("#e2ded6"), 2));
        p.setBrush(Qt::white);
        p.drawRoundedRect(row, 14, 14);
        p.setPen(QColor("#1d1c1a"));
        p.drawText(row.adjusted(30, 0, 0, 0), Qt::AlignVCenter, names[i]);
        p.setPen(QColor("#6f6b63"));
        p.drawText(row.adjusted(520, 0, 0, 0), Qt::AlignVCenter, keys[i]);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(i == 0 ? "#1f8f5f" : "#c9c4ba"));
        p.drawRoundedRect(QRectF(row.right() - 170, row.center().y() - 20, 130, 40), 20, 20);
    }
    p.setBrush(QColor("#1d1c1a"));
    p.drawRoundedRect(QRectF(window.left() + 480, window.top() + 700, 260, 70), 14, 14);
    p.setPen(Qt::white);
    p.drawText(QRectF(window.left() + 480, window.top() + 700, 260, 70), Qt::AlignCenter, QStringLiteral("Create key"));
    return image;
}

QAbstractButton *button(QWidget &window, const QString &name)
{
    for (auto *candidate : window.findChildren<QAbstractButton *>()) {
        if (candidate->text() == name || candidate->accessibleName() == name || candidate->toolTip().startsWith(name)) {
            return candidate;
        }
    }
    return nullptr;
}

void click(QWidget &window, const QString &name)
{
    if (auto *target = button(window, name)) {
        target->click();
    } else {
        qWarning("No button named %s", qPrintable(name));
    }
}

// Shows the editor's confirmation for Copy. Pressing the button would replace the clipboard of the
// desktop this runs on.
void showCopied(EditorWindow &editor)
{
    if (auto *notice = editor.findChild<Notice *>()) {
        notice->flash(QStringLiteral("Copied to clipboard"));
    } else {
        qWarning("The editor has no notice");
    }
}

QWidget *namedWidget(QWidget &window, const QString &name)
{
    for (auto *widget : window.findChildren<QWidget *>()) {
        if (widget->isVisible() && (widget->accessibleName() == name)) {
            return widget;
        }
    }
    qWarning("No widget named %s", qPrintable(name));
    return nullptr;
}

// Drags across the screenshot. Coordinates are fractions of the canvas widget.
void drag(QWidget *canvas, QPointF from, QPointF to, int steps = 2)
{
    const auto at = [canvas](QPointF f) { return QPoint(int(f.x() * canvas->width()), int(f.y() * canvas->height())); };
    QTest::mousePress(canvas, Qt::LeftButton, {}, at(from));
    for (int i = 1; i <= steps; ++i) {
        const QPointF f = from + (to - from) * (qreal(i) / steps);
        QTest::mouseMove(canvas, at(f));
        QTest::qWait(5);
    }
    QTest::mouseRelease(canvas, Qt::LeftButton, {}, at(to));
}

void editorShots()
{
    EditorWindow editor;
    editor.setShortcuts(QStringLiteral("⌘⌥⇧1"), QStringLiteral("⌘⌥⇧2"));
    present(editor, QSize(1180, 780));
    shoot(editor, QStringLiteral("01-launcher"));

    editor.setCapture(sampleCapture());
    shoot(editor, QStringLiteral("02-editor-fresh"));

    if (auto *canvas = namedWidget(editor, QStringLiteral("Screenshot"))) {
        drag(canvas, {0.20, 0.30}, {0.37, 0.50});
        click(editor, QStringLiteral("Rectangle"));
        drag(canvas, {0.30, 0.72}, {0.44, 0.83});
        click(editor, QStringLiteral("Redact"));
        drag(canvas, {0.52, 0.335}, {0.70, 0.385});
        click(editor, QStringLiteral("Blur"));
        drag(canvas, {0.52, 0.44}, {0.70, 0.50});
        click(editor, QStringLiteral("Pen"));
        drag(canvas, {0.75, 0.70}, {0.86, 0.78}, 12);
        click(editor, QStringLiteral("Arrow"));
    }
    shoot(editor, QStringLiteral("03-editor-annotated"));
    showCopied(editor);
    settle(150);
    shoot(editor, QStringLiteral("04-editor-copied"));

    editor.setShortcuts(QStringLiteral("⌘⌥⇧1"), {}, QStringLiteral("The region shortcut is already used by another application."));
    editor.setLibraryStatus(QStringLiteral("Library (3 pending): Could not reach the library over SSH. Check that library-server is online."));
    shoot(editor, QStringLiteral("05-editor-notices"));

    editor.resize(720, 500);
    shoot(editor, QStringLiteral("06-editor-small"));

    editor.setCapture(sampleCapture(QSize(620, 240)));
    editor.setLibraryStatus({});
    editor.setShortcuts(QStringLiteral("⌘⌥⇧1"), QStringLiteral("⌘⌥⇧2"));
    editor.resize(1180, 780);
    shoot(editor, QStringLiteral("07-editor-small-capture"));
}

void settingsShots()
{
    {
        SettingsWindow settings(AppSettings{});
        present(settings, settings.size());
        shoot(settings, QStringLiteral("10-settings-standalone"));
    }
    {
        SettingsWindow settings(AppSettings{});
        settings.findChild<Segmented *>(QStringLiteral("libraryConnection"))->setCurrent(1);
        settings.findChild<QLineEdit *>(QStringLiteral("libraryDevice"))->setText(QStringLiteral("Home computer"));
        present(settings, settings.size());
        shoot(settings, QStringLiteral("15-settings-create"));
        settings.findChild<Segmented *>(QStringLiteral("libraryConnection"))->setCurrent(2);
        TailscaleStatus missing;
        missing.state = TailscaleState::Missing;
        missing.message = QStringLiteral("Install Tailscale on both computers and sign in with the same account.");
        settings.setTailscaleStatus(missing);
        shoot(settings, QStringLiteral("16-settings-connect"));
    }
    {
        AppSettings hosting;
        hosting.hostedLibrary = {true, QDir::home().filePath(QStringLiteral(".frameline/library")),
                                 QString(43, QLatin1Char('a')), defaultHostedLibraryPort, true};
        hosting.library.token = hosting.hostedLibrary.token;
        hosting.library.port = hosting.hostedLibrary.port;
        hosting.library.device = QStringLiteral("Home computer");
        hosting.library.autoArchive = true;
        SettingsWindow settings(hosting);
        TailscaleStatus connected;
        connected.state = TailscaleState::Ready;
        connected.localAddress = QStringLiteral("100.100.100.100");
        connected.message = QStringLiteral("Tailscale is connected.");
        settings.setTailscaleStatus(connected);
        settings.setHostedStatus(true, connected.localAddress);
        present(settings, settings.size());
        shoot(settings, QStringLiteral("17-settings-host-ready"));
    }
    AppSettings configured;
    configured.library.transport = LibraryTransport::Ssh;
    configured.library.sshHost = QStringLiteral("library-server");
    configured.library.device = QStringLiteral("Work laptop");
    configured.library.token = QString(32, QLatin1Char('a'));
    configured.library.autoArchive = true;
    SettingsWindow settings(configured);
    present(settings, settings.size());
    shoot(settings, QStringLiteral("11-settings-ssh"));
    click(settings, QStringLiteral("Test connection"));
    shoot(settings, QStringLiteral("12-settings-testing"));
    settings.setTestResult({});
    shoot(settings, QStringLiteral("13-settings-connected"));
    if (auto *host = settings.findChild<QLineEdit *>(QStringLiteral("libraryHost"))) {
        host->clear();
    }
    click(settings, QStringLiteral("Save"));
    shoot(settings, QStringLiteral("14-settings-error"));
}

void libraryShots(int port, const QString &token)
{
    QTemporaryDir directory;
    LibraryClient history(directory.path(), {});
    history.start();
    {
        LibraryWindow library(history);
        present(library, QSize(980, 700));
        shoot(library, QStringLiteral("20-library-unconfigured"));
    }
    if (token.isEmpty()) {
        return;
    }
    AppSettings settings;
    settings.library.transport = LibraryTransport::Local;
    settings.library.port = port;
    settings.library.token = token;
    settings.library.device = QStringLiteral("Work laptop");
    settings.library.autoArchive = true;
    saveSettings(settings, directory.path());
    const auto showShared = [](LibraryWindow &library) {
        present(library, QSize(980, 700));
        if (auto *source = library.findChild<Segmented *>(QStringLiteral("librarySource"))) {
            source->setCurrent(1);
        }
        settle(1500);
    };
    {
        LibraryClient client(settings.library, directory.path());
        client.start();
        LibraryWindow library(history, nullptr, &client);
        showShared(library);
        shoot(library, QStringLiteral("21-library-grid"));
        if (auto *list = library.findChild<QListView *>()) {
            if (list->model() && list->model()->rowCount() > 1) {
                const QModelIndex index = list->model()->index(1, 0);
                list->setCurrentIndex(index);
                emit list->activated(index);
            }
        }
        settle(1200);
        shoot(library, QStringLiteral("22-library-viewer"));
        library.resize(620, 460);
        shoot(library, QStringLiteral("23-library-viewer-small"));
    }
    settings.library.port = port + 1;
    saveSettings(settings, directory.path());
    LibraryClient client(settings.library, directory.path());
    client.start();
    LibraryWindow library(history, nullptr, &client);
    showShared(library);
    shoot(library, QStringLiteral("24-library-unreachable"));
}

// A grab leaves the pointer out. Record its position in PNG pixels so the compositor
// resizes the pointer and image with the same transform, even on a high-DPI screen.
class Recorder {
public:
    Recorder(const QString &directory, int first) : directory(directory), index(first), log(directory + QStringLiteral("/frames.txt"))
    {
        QDir().mkpath(directory);
        const QByteArray header = "# freezecut-frame-coordinates: png-pixels\n";
        if (!log.open(QIODevice::WriteOnly | QIODevice::Text) || log.write(header) != header.size()) {
            qWarning("Cannot write %s", qPrintable(log.fileName()));
            saveFailed = true;
        }
    }

    QPoint pointer;
    const char *cursor = "arrow";
    int index;

    void frame(QWidget &window)
    {
        const QPixmap image = save(window, QStringLiteral("%1/%2.png").arg(directory).arg(index, 4, 10, QLatin1Char('0')));
        const QPointF pixel = QPointF(pointer) * image.devicePixelRatio();
        const QByteArray entry = QStringLiteral("%1 %2 %3 %4\n")
            .arg(index).arg(pixel.x(), 0, 'f', 3).arg(pixel.y(), 0, 'f', 3).arg(QString::fromLatin1(cursor)).toUtf8();
        if (log.isOpen() && (log.write(entry) != entry.size() || !log.flush())) {
            qWarning("Could not write frame %d to %s", index, qPrintable(log.fileName()));
            saveFailed = true;
        }
        ++index;
    }

    void hold(QWidget &window, int frames)
    {
        for (int i = 0; i < frames; ++i) {
            QTest::qWait(66);
            frame(window);
        }
    }

    // Moves the pointer along an eased path, delivering the moves so hover states react.
    void glide(QWidget &window, QWidget *target, QPoint to, int frames, const char *shape = "arrow")
    {
        const QPoint from = pointer;
        QEasingCurve curve(QEasingCurve::InOutCubic);
        for (int i = 1; i <= frames; ++i) {
            const qreal t = curve.valueForProgress(qreal(i) / frames);
            pointer = from + (to - from) * t;
            cursor = shape;
            if (target) {
                QTest::mouseMove(target, target->mapFrom(&window, pointer));
            }
            QTest::qWait(30);
            frame(window);
        }
    }

private:
    QString directory;
    QFile log;
};

void welcomeDemo(const QString &directory)
{
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/freezecut.png")));
    WelcomeWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    Recorder record(directory, 0);
    for (int step = 0; step < 2; ++step) {
        QElapsedTimer elapsed;
        elapsed.start();
        for (int frame = 0; frame < 42; ++frame) {
            record.frame(window);
            settle(int(qMax(qint64(0), (frame + 1) * 33 - elapsed.elapsed())));
        }
        if (step == 0)
            click(window, QStringLiteral("Continue"));
    }
}

QPoint centerOf(QWidget &window, QWidget *widget)
{
    return widget ? widget->mapTo(&window, widget->rect().center()) : QPoint();
}

const QSize demoSize(1180, 780);
const QSize demoCapture(2360, 1560);
const QRect demoSelection(140, 66, 900, 586);

// Opens the region overlay on a sample capture and returns its window at the given size.
QWidget *openRegionOverlay(QSize capture, QSize window)
{
    RegionSelector::start({CaptureFrame{sampleCapture(capture), nullptr}}, [](QImage, const auto &) {});
    settle(300);
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (widget->isVisible() && widget->windowTitle().contains(QStringLiteral("Select region"))) {
            widget->resize(window);
            settle();
            return widget;
        }
    }
    qWarning("The region overlay did not open");
    return nullptr;
}

void regionDemo(const QString &directory)
{
    Recorder record(directory, 100);
    QWidget *overlay = openRegionOverlay(demoCapture, demoSize);
    if (!overlay) {
        return;
    }
    record.pointer = QPoint(640, 470);
    record.cursor = "cross";
    QTest::mouseMove(overlay, record.pointer);
    record.hold(*overlay, 4);
    record.glide(*overlay, overlay, demoSelection.topLeft(), 14, "cross");
    record.hold(*overlay, 3);
    QTest::mousePress(overlay, Qt::LeftButton, {}, demoSelection.topLeft());
    QEasingCurve curve(QEasingCurve::InOutCubic);
    const QPoint dragDistance(demoSelection.width(), demoSelection.height());
    for (int i = 1; i <= 22; ++i) {
        const qreal t = curve.valueForProgress(i / 22.0);
        record.pointer = demoSelection.topLeft() + dragDistance * t;
        QTest::mouseMove(overlay, record.pointer);
        QTest::qWait(30);
        record.frame(*overlay);
    }
    record.hold(*overlay, 8);
    QTest::keyClick(overlay, Qt::Key_Escape);
    settle(100);
}

void editorDemo(const QString &directory)
{
    Recorder record(directory, 0);
    EditorWindow editor;
    editor.setShortcuts(QStringLiteral("⌘⌥⇧1"), QStringLiteral("⌘⌥⇧2"));
    present(editor, demoSize);
    settle();

    // The start screen: reach for Select region.
    QWidget *hero = namedWidget(editor, QStringLiteral("Select screen region"));
    record.pointer = QPoint(930, 640);
    record.hold(editor, 6);
    record.glide(editor, &editor, centerOf(editor, hero), 14);
    record.hold(editor, 4);
    if (hero) {
        QTest::mousePress(hero, Qt::LeftButton, {}, hero->rect().center());
        record.hold(editor, 2);
        QTest::mouseRelease(hero, Qt::LeftButton, {}, hero->rect().center());
    }

    // The selected region opens in the editor.
    record.index = 1000;
    const qreal scale = qreal(demoCapture.width()) / demoSize.width();
    const QRect crop(demoSelection.topLeft() * scale,
                     demoSelection.size() * scale);
    editor.setCapture(sampleCapture(demoCapture).copy(crop));
    settle(100);
    QWidget *canvas = namedWidget(editor, QStringLiteral("Screenshot"));
    if (!canvas) {
        return;
    }
    const auto at = [&](qreal x, qreal y) { return canvas->mapTo(&editor, QPoint(int(x * canvas->width()), int(y * canvas->height()))); };
    record.pointer = at(0.62, 0.86);
    record.cursor = "cross";
    record.hold(editor, 10);

    const auto markWith = [&](const QString &tool, QPointF from, QPointF to, int steps) {
        QWidget *button = namedWidget(editor, tool);
        record.glide(editor, &editor, centerOf(editor, button), 10);
        if (auto *toolButton = qobject_cast<QAbstractButton *>(button)) {
            toolButton->click();
        }
        record.hold(editor, 4);
        record.glide(editor, nullptr, at(from.x(), from.y()), 10, "cross");
        QTest::mousePress(canvas, Qt::LeftButton, {}, canvas->mapFrom(&editor, record.pointer));
        QEasingCurve curve(QEasingCurve::InOutCubic);
        const QPoint start = record.pointer;
        const QPoint end = at(to.x(), to.y());
        for (int i = 1; i <= steps; ++i) {
            record.pointer = start + (end - start) * curve.valueForProgress(qreal(i) / steps);
            QTest::mouseMove(canvas, canvas->mapFrom(&editor, record.pointer));
            QTest::qWait(30);
            record.frame(editor);
        }
        QTest::mouseRelease(canvas, Qt::LeftButton, {}, canvas->mapFrom(&editor, record.pointer));
        record.hold(editor, 4);
    };
    markWith(QStringLiteral("Redact"), {0.522, 0.192}, {0.735, 0.243}, 12);
    markWith(QStringLiteral("Blur"), {0.522, 0.282}, {0.735, 0.525}, 12);
    markWith(QStringLiteral("Arrow"), {0.60, 0.80}, {0.405, 0.635}, 14);

    record.glide(editor, &editor, centerOf(editor, namedWidget(editor, QStringLiteral("Copy"))), 12);
    record.hold(editor, 2);
    showCopied(editor);
    // End while the confirmation is still showing; the last frame is held when compositing.
    record.hold(editor, 5);
}

// The overlay needs the offscreen platform, so it renders in a separate run: freezecut_shots <dir> --region
void regionShots()
{
    QWidget *overlay = openRegionOverlay(QSize(1600, 1000), QSize(1600, 1000));
    if (!overlay) {
        return;
    }
    QTest::mouseMove(overlay, QPoint(620, 430));
    shoot(*overlay, QStringLiteral("30-region-idle"));
    QTest::mousePress(overlay, Qt::LeftButton, {}, QPoint(480, 260));
    QTest::mouseMove(overlay, QPoint(900, 480));
    QTest::mouseMove(overlay, QPoint(1180, 620));
    shoot(*overlay, QStringLiteral("31-region-dragging"));
    QTest::keyClick(overlay, Qt::Key_Escape);
    settle(100);
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() < 2) {
        qWarning("Usage: freezecut_shots <output-directory> "
                 "[<library-port> <library-token> | --demo-welcome | --region | --demo-editor | --demo-region]");
        return 1;
    }
    outputDirectory = arguments.at(1);
    QDir().mkpath(outputDirectory);
    if (arguments.value(2) == QStringLiteral("--region")) {
        regionShots();
    } else if (arguments.value(2) == QStringLiteral("--demo-region")) {
        regionDemo(outputDirectory);
    } else if (arguments.value(2) == QStringLiteral("--demo-editor")) {
        editorDemo(outputDirectory);
    } else if (arguments.value(2) == QStringLiteral("--demo-welcome")) {
        welcomeDemo(outputDirectory);
    } else {
        editorShots();
        settingsShots();
        libraryShots(arguments.value(2).toInt(), arguments.value(3));
    }
    return saveFailed ? 1 : 0;
}
