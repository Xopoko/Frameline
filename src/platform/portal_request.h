#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QVariant>

namespace Portal {
inline const auto service = QStringLiteral("org.freedesktop.portal.Desktop");
inline const auto path = QStringLiteral("/org/freedesktop/portal/desktop");
inline const auto sessionInterface = QStringLiteral("org.freedesktop.portal.Session");

bool isWayland();
QString token();
QDBusConnection connection();

class Request : public QObject {
    Q_OBJECT

public:
    explicit Request(QObject *parent);
    ~Request() override;
    void start(const QString &interface, const QString &method, QVariantList arguments,
               QVariantMap options = {});

signals:
    void responded(uint code, const QVariantMap &results);
    void failed();

private slots:
    void receiveResponse(uint code, const QVariantMap &results);

private:
    bool subscribe(const QString &path);
    void unsubscribe();
    void fail();

    QDBusConnection bus_;
    QString path_;
    bool finished_ = false;
};
} // namespace Portal
