#include "pngexport.h"

#include <QImage>
#include <QImageWriter>
#include <QSaveFile>

bool savePng(const QImage &image, const QString &path, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return false;
    }

    // Qt's PNG writer otherwise allocates and converts one image per scanline.
    QImage pixels = image;
    if (image.format() == QImage::Format_ARGB32_Premultiplied) {
        pixels = image.convertToFormat(QImage::Format_ARGB32);
    }
    QImageWriter writer(&file, "png");
    if (!writer.write(pixels)) {
        *error = writer.errorString();
        return false;
    }
    if (!file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}
