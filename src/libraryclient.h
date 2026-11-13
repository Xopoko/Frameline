#pragma once

#include <QDateTime>
#include <QImage>
#include <QList>
#include <QObject>

#include <functional>
#include <memory>

struct LibrarySettings;

struct LibraryItem {
    QString id;
    QDateTime capturedAt;
    QSize size;
    QString device;
    QString kind;
    bool hasThumbnail = false;
};

class LibraryClient final : public QObject {
    Q_OBJECT

public:
    using ListCallback = std::function<void(QList<LibraryItem>, QString nextCursor, QString error)>;
    using ImageCallback = std::function<void(QImage, QString error)>;

    explicit LibraryClient(const LibrarySettings &settings, QObject *parent = nullptr);
    ~LibraryClient() override;

    bool isConfigured() const;
    int pendingCount() const;
    void start();
    void enqueue(QImage image, const QString &kind, const QDateTime &capturedAt);
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
