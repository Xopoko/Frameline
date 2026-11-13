#include "annotations.h"
#include "pngexport.h"

#include <QApplication>
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
#if defined(Q_OS_WIN)
    const bool shortcutPassed = windowsRegionKey();
#else
    const bool shortcutPassed = true;
#endif
    return annotationsPassed && damagePassed && exportPassed && shortcutPassed ? 0 : 1;
}
