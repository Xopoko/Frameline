#include "librarystorage.h"

#include "libraryprotocol.h"
#include "pngexport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <optional>
#include <utility>

namespace {

const auto imageName = QStringLiteral("image.png");
const auto thumbnailName = QStringLiteral("thumbnail.png");
const auto legacyThumbnailName = QStringLiteral("thumbnail.jpg");
const auto metadataName = QStringLiteral("metadata.json");
constexpr int maxMetadataBytes = 2048;
constexpr int pageSize = 50;

bool isLink(const QFileInfo &entry)
{
    return entry.isSymLink() || entry.isJunction();
}

bool isRegularFile(const QFileInfo &entry)
{
    return entry.isFile() && !isLink(entry) && entry.size() > 0;
}

bool canReadDirectory(const QFileInfo &entry)
{
    if (!entry.isDir() || isLink(entry) || !entry.isReadable())
        return false;
#ifdef Q_OS_WIN
    return true;
#else
    return entry.isExecutable();
#endif
}

bool writeMetadata(const QString &path, const QJsonObject &metadata)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QByteArray bytes = QJsonDocument(metadata).toJson(QJsonDocument::Compact);
    return file.write(bytes) == bytes.size() && file.commit();
}

struct Entry {
    LibraryItem item;
    QString original;
    QString thumbnail;
};

std::optional<Entry> readEntry(const QDir &directory, const QString &id)
{
    const QString path = directory.filePath(id);
    if (!canReadDirectory(QFileInfo(path)))
        return std::nullopt;

    const QDir capture(path);
    const QFileInfo original(capture.filePath(imageName));
    const QFileInfo metadata(capture.filePath(metadataName));
    QFile file(metadata.filePath());
    if (!isRegularFile(original) || !isRegularFile(metadata) || metadata.size() > maxMetadataBytes
        || !file.open(QIODevice::ReadOnly))
        return std::nullopt;

    const auto fields = QJsonDocument::fromJson(file.read(maxMetadataBytes)).object();
    const auto capturedAt = QDateTime::fromString(fields.value(QStringLiteral("capturedAt")).toString(), Qt::ISODateWithMs);
    const QSize size(fields.value(QStringLiteral("width")).toInt(), fields.value(QStringLiteral("height")).toInt());
    const auto kind = fields.value(QStringLiteral("kind")).toString();
    if (!capturedAt.isValid() || !LibraryProtocol::isImageSize(size)
        || (kind != QStringLiteral("full") && kind != QStringLiteral("region")))
        return std::nullopt;

    QString thumbnail;
    for (const auto &name : {thumbnailName, legacyThumbnailName}) {
        const QFileInfo candidate(capture.filePath(name));
        if (isRegularFile(candidate)) {
            thumbnail = candidate.filePath();
            break;
        }
    }
    return Entry{{id, capturedAt.toUTC(), size, fields.value(QStringLiteral("device")).toString(), kind, !thumbnail.isEmpty()},
                 original.filePath(), thumbnail};
}

using SortKey = std::pair<QDateTime, QString>;

SortKey key(const LibraryItem &item)
{
    return {item.capturedAt, item.id};
}

std::optional<SortKey> readCursor(const QString &cursor)
{
    const auto separator = cursor.indexOf(u'/');
    if (separator < 0)
        return std::nullopt;
    const auto timestamp = cursor.left(separator);
    const auto date = QDateTime::fromString(timestamp, Qt::ISODateWithMs);
    const auto id = cursor.mid(separator + 1);
    if (!date.isValid() || date.toUTC().toString(Qt::ISODateWithMs) != timestamp || !LibraryProtocol::isScreenshotId(id))
        return std::nullopt;
    return SortKey{date, id};
}

} // namespace

QString LibraryStorage::writeCapture(const QString &directory, const QString &id, const QImage &image,
                                     const QJsonObject &metadata, bool uploadQueue)
{
    if (!LibraryProtocol::isScreenshotId(id))
        return QStringLiteral("Invalid screenshot ID.");
    if (!LibraryProtocol::isImageSize(image.size()))
        return uploadQueue ? QStringLiteral("The screenshot is too large for the shared library.")
                           : QStringLiteral("The screenshot is too large to save in Library.");
    if (!QDir::isAbsolutePath(directory) || QDir(directory).isRoot() || !QDir().mkpath(directory)
        || isLink(QFileInfo(directory)))
        return uploadQueue ? QStringLiteral("Could not create the local screenshot queue.")
                           : QStringLiteral("Could not create the screenshot history folder.");
#ifndef Q_OS_WIN
    if (!QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return uploadQueue ? QStringLiteral("Could not make the local screenshot queue private.")
                           : QStringLiteral("Could not make the screenshot history folder private.");
#endif
    const QDir destination(directory);
    QTemporaryDir staging(destination.filePath(QStringLiteral(".staging-XXXXXX")));
    if (!staging.isValid())
        return QStringLiteral("Could not create a temporary folder for the screenshot.");

    QString error;
    const QString original = staging.filePath(imageName);
    if (!savePng(image, original, &error))
        return uploadQueue ? QStringLiteral("Could not queue the screenshot. %1").arg(error)
                           : QStringLiteral("Could not save the screenshot in Library. %1").arg(error);
    if (uploadQueue && QFileInfo(original).size() > LibraryProtocol::maxImageBytes)
        return QStringLiteral("The encoded screenshot exceeds the library's 64 MiB limit.");

    const QImage thumbnail = image.width() > 360 || image.height() > 240
        ? image.scaled(360, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation) : image;
    if (!savePng(thumbnail, staging.filePath(thumbnailName), &error))
        return QStringLiteral("Could not save the screenshot preview. %1").arg(error);
    if (!writeMetadata(staging.filePath(metadataName), metadata))
        return QStringLiteral("Could not save the screenshot metadata.");
    if (!QDir().rename(staging.path(), destination.filePath(id)))
        return uploadQueue ? QStringLiteral("Could not add the screenshot to the local queue.")
                           : QStringLiteral("Could not add the screenshot to Library.");
    staging.setAutoRemove(false);
    return {};
}

LibraryStorage::Page LibraryStorage::list(const QStringList &directories, const QString &cursor)
{
    const auto before = readCursor(cursor);
    if (!cursor.isEmpty() && !before)
        return {{}, {}, QStringLiteral("Invalid screenshot history page.")};

    QSet<QString> seen;
    Page page;
    for (qsizetype index = 0; index < directories.size(); ++index) {
        const QString &path = directories.at(index);
        const QFileInfo root(path);
        if (!root.exists() && !isLink(root))
            continue;
        if (!QDir::isAbsolutePath(path) || QDir(path).isRoot() || !canReadDirectory(root)) {
            if (index == 0)
                return {{}, {}, QStringLiteral("Could not read the screenshot history folder.")};
            continue;
        }
        const QDir directory(path);
        for (const auto &id : directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::NoSort)) {
            if (!LibraryProtocol::isScreenshotId(id) || seen.contains(id))
                continue;
            auto entry = readEntry(directory, id);
            if (!entry)
                continue;
            seen.insert(id);
            if (!before || key(entry->item) < *before)
                page.items.append(std::move(entry->item));
        }
    }

    const auto newestFirst = [](const LibraryItem &left, const LibraryItem &right) { return key(left) > key(right); };
    if (page.items.size() > pageSize) {
        std::partial_sort(page.items.begin(), page.items.begin() + pageSize, page.items.end(), newestFirst);
        page.items.resize(pageSize);
        const auto &last = page.items.constLast();
        page.nextCursor = last.capturedAt.toString(Qt::ISODateWithMs) + u'/' + last.id;
    } else {
        std::sort(page.items.begin(), page.items.end(), newestFirst);
    }
    return page;
}

LibraryStorage::Image LibraryStorage::image(const QStringList &directories, const QString &id, bool thumbnail)
{
    if (!LibraryProtocol::isScreenshotId(id))
        return {{}, QStringLiteral("Invalid screenshot ID.")};

    for (const auto &path : directories) {
        const QDir directory(path);
        if (!QDir::isAbsolutePath(path) || directory.isRoot() || !canReadDirectory(QFileInfo(path)))
            continue;
        const auto entry = readEntry(directory, id);
        if (!entry)
            continue;
        const QString &file = thumbnail ? entry->thumbnail : entry->original;
        if (file.isEmpty())
            return {{}, QStringLiteral("The screenshot preview is unavailable.")};
        QImageReader reader(file);
        const QSize size = reader.size();
        if (!LibraryProtocol::isImageSize(size) || (thumbnail ? std::max(size.width(), size.height()) > 1024
                                                            : size != entry->item.size))
            return {{}, QStringLiteral("The screenshot file has invalid dimensions.")};
        QImage decoded = reader.read();
        if (decoded.isNull())
            return {{}, QStringLiteral("Could not read the screenshot. %1").arg(reader.errorString())};
        return {std::move(decoded), {}};
    }
    return {{}, QStringLiteral("The screenshot is no longer available.")};
}
