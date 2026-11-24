#pragma once

#include <QDateTime>
#include <QImage>
#include <QList>
#include <QStringList>

class QJsonObject;

struct LibraryItem {
    QString id;
    QDateTime capturedAt;
    QSize size;
    QString device;
    QString kind;
    bool hasThumbnail = false;
};

namespace LibraryStorage {

QString writeCapture(const QString &directory, const QString &id, const QImage &image,
                     const QJsonObject &metadata, bool uploadQueue);

struct Page {
    QList<LibraryItem> items;
    QString nextCursor;
    QString error;
};

struct Image {
    QImage image;
    QString error;
};

// The local history is first, followed by any older libraries kept on this device.
Page list(const QStringList &directories, const QString &cursor);
Image image(const QStringList &directories, const QString &id, bool thumbnail);

} // namespace LibraryStorage
