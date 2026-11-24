#include "librarystorage.h"

#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QString captureId(int number)
{
    return QStringLiteral("00000000-0000-0000-0000-%1").arg(number, 12, 16, QLatin1Char('0'));
}

const auto captureTime = QDateTime::fromString(QStringLiteral("2026-11-24T12:00:00.000Z"), Qt::ISODateWithMs);

QJsonObject metadata(const QImage &image, const QDateTime &time, const QString &device)
{
    return {{QStringLiteral("capturedAt"), time.toString(Qt::ISODateWithMs)},
            {QStringLiteral("device"), device}, {QStringLiteral("kind"), QStringLiteral("region")},
            {QStringLiteral("width"), image.width()}, {QStringLiteral("height"), image.height()}};
}

} // namespace

class LibraryStorageTests final : public QObject {
    Q_OBJECT

private slots:
    void persistsAndReopensCapture();
    void readsLegacyAlongsideStablePages();
};

void LibraryStorageTests::persistsAndReopensCapture()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const QString history = profile.filePath(QStringLiteral("history"));
    const auto empty = LibraryStorage::list({history}, {});
    QVERIFY(empty.error.isEmpty());
    QVERIFY(empty.items.isEmpty());

    QImage image(512, 320, QImage::Format_RGB32);
    image.fill(Qt::blue);
    const auto id = captureId(1);
    const auto device = QStringLiteral("This device");
    const auto error = LibraryStorage::writeCapture(history, id, image, metadata(image, captureTime, device), false);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const auto saved = LibraryStorage::list({history}, {});
    QVERIFY2(saved.error.isEmpty(), qPrintable(saved.error));
    QCOMPARE(saved.items.size(), 1);
    QCOMPARE(saved.items.constFirst().id, id);
    QCOMPARE(saved.items.constFirst().capturedAt, captureTime);
    QCOMPARE(saved.items.constFirst().device, device);
    QCOMPARE(saved.items.constFirst().size, image.size());
    QVERIFY(saved.items.constFirst().hasThumbnail);
    QVERIFY(saved.nextCursor.isEmpty());

    const auto opened = LibraryStorage::image({history}, id, false);
    QVERIFY2(opened.error.isEmpty(), qPrintable(opened.error));
    QCOMPARE(opened.image, image);
    const auto thumbnail = LibraryStorage::image({history}, id, true);
    QVERIFY2(thumbnail.error.isEmpty(), qPrintable(thumbnail.error));
    QCOMPARE(thumbnail.image.size(), image.size().scaled(360, 240, Qt::KeepAspectRatio));

    const QString fileInsteadOfDirectory = profile.filePath(QStringLiteral("unavailable"));
    QFile unavailable(fileInsteadOfDirectory);
    QVERIFY(unavailable.open(QIODevice::WriteOnly));
    unavailable.close();
    QVERIFY(!LibraryStorage::list({fileInsteadOfDirectory}, {}).error.isEmpty());
}

void LibraryStorageTests::readsLegacyAlongsideStablePages()
{
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const QString history = profile.filePath(QStringLiteral("history"));
    const QString legacy = profile.filePath(QStringLiteral("library"));
    const QStringList directories{history, legacy};
    QImage image(16, 12, QImage::Format_RGB32);
    image.fill(Qt::blue);
    const auto localFields = metadata(image, captureTime, QStringLiteral("Local"));
    for (int index = 1; index <= 50; ++index) {
        const auto error = LibraryStorage::writeCapture(history, captureId(index), image, localFields, false);
        QVERIFY2(error.isEmpty(), qPrintable(error));
    }

    image.fill(Qt::red);
    auto legacyFields = metadata(image, captureTime, QStringLiteral("Legacy"));
    legacyFields.insert(QStringLiteral("imageSha256"), QString(64, QLatin1Char('a')));
    const auto legacyId = captureId(0);
    const auto error = LibraryStorage::writeCapture(legacy, legacyId, image, legacyFields, true);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto duplicate = LibraryStorage::writeCapture(legacy, captureId(1), image, legacyFields, true);
    QVERIFY2(duplicate.isEmpty(), qPrintable(duplicate));
    const QString assetPrefix = legacy + u'/' + legacyId + u'/';
    QVERIFY(QFile::remove(assetPrefix + QStringLiteral("thumbnail.png")));
    QVERIFY(image.save(assetPrefix + QStringLiteral("thumbnail.jpg"), "JPEG"));

    const auto first = LibraryStorage::list(directories, {});
    QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
    QCOMPARE(first.items.size(), 50);
    QCOMPARE(first.items.constFirst().id, captureId(50));
    QCOMPARE(first.items.constLast().id, captureId(1));
    QCOMPARE(first.items.constLast().device, QStringLiteral("Local"));
    QVERIFY(!first.nextCursor.isEmpty());

    const auto fresh = LibraryStorage::writeCapture(history, captureId(51), image, localFields, false);
    QVERIFY2(fresh.isEmpty(), qPrintable(fresh));
    const auto second = LibraryStorage::list(directories, first.nextCursor);
    QVERIFY2(second.error.isEmpty(), qPrintable(second.error));
    QCOMPARE(second.items.size(), 1);
    QCOMPARE(second.items.constFirst().id, legacyId);
    QVERIFY(second.items.constFirst().hasThumbnail);
    QVERIFY(second.nextCursor.isEmpty());
    const auto thumbnail = LibraryStorage::image(directories, legacyId, true);
    QVERIFY2(thumbnail.error.isEmpty(), qPrintable(thumbnail.error));
    QCOMPARE(thumbnail.image.size(), image.size());
    const auto localImage = LibraryStorage::image(directories, captureId(1), false);
    QVERIFY2(localImage.error.isEmpty(), qPrintable(localImage.error));
    QCOMPARE(localImage.image.pixelColor(0, 0), QColor(Qt::blue));
}

QTEST_GUILESS_MAIN(LibraryStorageTests)
#include "librarystorage_tests.moc"
