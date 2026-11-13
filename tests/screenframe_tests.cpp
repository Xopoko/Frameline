#include "annotations.h"
#include "editor.h"

#include <QApplication>
#include <QTemporaryDir>
#include <QTextStream>

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
    return passed;
}

} // namespace

int main(int argc, char **argv)
{
    const QApplication application(argc, argv);
    const bool annotationsPassed = annotationRendering();
    const bool exportPassed = pngExport();
    return annotationsPassed && exportPassed ? 0 : 1;
}
