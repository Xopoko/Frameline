#pragma once

#include "librarystorage.h"
#include "settings.h"

#include <QDateTime>
#include <QImage>
#include <QList>
#include <QObject>

#include <functional>
#include <memory>

class LibraryClient final : public QObject {
    Q_OBJECT

public:
    using ListCallback = std::function<void(QList<LibraryItem>, QString nextCursor, QString error)>;
    using ImageCallback = std::function<void(QImage, QString error)>;

    explicit LibraryClient(const LibrarySettings &settings, const QString &settingsDirectory = defaultSettingsDirectory());
    LibraryClient(const QString &settingsDirectory, const QString &hostedDirectory);
    ~LibraryClient() override;

    bool isConfigured() const;
    bool isLocalHistory() const;
    void setHostedHistoryDirectory(const QString &directory);
    int pendingCount() const;
    void start();
    // A hosted library can still queue captures when it cannot listen.
    void setConnectionError(const QString &error);
    QString enqueue(QImage image, const QString &kind, const QDateTime &capturedAt, const QString &id = {});
    bool flushPendingWrites();
    void list(const QString &cursor, QObject *context, ListCallback callback);
    void image(const QString &id, bool thumbnail, QObject *context, ImageCallback callback);

signals:
    void libraryChanged();
    void syncStatusChanged(int pending, const QString &error);

private:
    struct State;
    std::unique_ptr<State> d;
};
