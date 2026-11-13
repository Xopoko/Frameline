#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QScreen>

class QWidget;

struct CaptureFrame {
    QImage image;
    QPointer<QScreen> screen;
};

class ScreenCapture : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    static bool usesScreenshotPortal();
    void capture(bool region, QWidget *portalParent = nullptr);

signals:
    void captured(const QList<CaptureFrame> &frames);
    void failed(const QString &message);
    void portalPermissionRequired(const QString &message);
    void cancelled();
};
