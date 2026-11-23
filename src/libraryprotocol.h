#pragma once

#include <QRegularExpression>
#include <QSize>
#include <QString>

// What the clients and servers of the library API agree on. The standalone server in
// server/frameline_server.py enforces the same rules.
namespace LibraryProtocol {

inline constexpr qint64 maxImageBytes = 64 * 1024 * 1024;
inline constexpr qint64 maxThumbnailBytes = 2 * 1024 * 1024;

inline bool isToken(const QString &token)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{32,256}\\z"));
    return pattern.match(token).hasMatch();
}

// A screenshot is identified by a lowercase UUID.
inline bool isScreenshotId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\\z"));
    return pattern.match(id).hasMatch();
}

inline bool isImageSize(const QSize &size)
{
    return size.width() > 0 && size.height() > 0 && size.width() <= 32768 && size.height() <= 32768
        && qint64(size.width()) * size.height() <= 100'000'000;
}

} // namespace LibraryProtocol
