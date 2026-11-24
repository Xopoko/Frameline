#include "pngexport.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QImage>
#include <QImageWriter>
#include <QMessageBox>
#include <QSaveFile>
#include <QStandardPaths>

namespace {

bool writePng(const QImage &image, QSaveFile &file, QString *error)
{
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

} // namespace

bool savePng(const QImage &image, const QString &path, QString *error)
{
    QSaveFile file(path);
    return writePng(image, file, error);
}

bool saveImageAs(QWidget *parent, const QImage &image, const QDateTime &capturedAt)
{
    QString directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (directory.isEmpty() || !QDir(directory).exists()) {
        directory = QDir::homePath();
    }
    QFileDialog dialog(parent, QStringLiteral("Save screenshot"), directory, QStringLiteral("PNG (*.png)"));
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QStringLiteral("png"));
    const QString timestamp = capturedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    dialog.selectFile(QStringLiteral("Screenshot_%1.png").arg(timestamp));
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
        return false;
    }
    QString error;
    QSaveFile file(dialog.selectedFiles().constFirst());
#ifdef FREEZECUT_MAC_APP_STORE
    // The save panel may grant access to only the selected file, not a temporary sibling.
    file.setDirectWriteFallback(true);
#endif
    if (!writePng(image, file, &error)) {
        QMessageBox::warning(parent, QStringLiteral("Could not save screenshot"), error);
        return false;
    }
    return true;
}
