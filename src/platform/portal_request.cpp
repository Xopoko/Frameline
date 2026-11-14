#include "portal_request.h"

#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QGuiApplication>
#include <QTimer>
#include <QUuid>

namespace Portal {
namespace {
const auto requestInterface = QStringLiteral("org.freedesktop.portal.Request");
const auto responseSignal = QStringLiteral("Response");
} // namespace

bool isWayland()
{
    return QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))
        || qEnvironmentVariable("XDG_SESSION_TYPE") == QStringLiteral("wayland");
}

QString token()
{
    return QStringLiteral("frameline_") + QUuid::createUuid().toString(QUuid::Id128);
}

QDBusConnection connection()
{
    auto bus = QDBusConnection::sessionBus();
    static bool registered = false;
    static auto *watcher = new QDBusServiceWatcher(
        service, bus, QDBusServiceWatcher::WatchForUnregistration, qApp);
    static const auto resetRegistration = QObject::connect(
        watcher, &QDBusServiceWatcher::serviceUnregistered, [] { registered = false; });
    Q_UNUSED(resetRegistration);

    if (bus.isConnected() && !registered && !QGuiApplication::desktopFileName().isEmpty()) {
        auto message = QDBusMessage::createMethodCall(
            service, path, QStringLiteral("org.freedesktop.host.portal.Registry"),
            QStringLiteral("Register"));
        message.setArguments({QGuiApplication::desktopFileName(), QVariantMap{}});
        // D-Bus preserves message order for a connection. Older portals can ignore Register.
        registered = bus.send(message);
    }
    return bus;
}

Request::Request(QObject *parent)
    : QObject(parent), bus_(connection())
{
}

Request::~Request()
{
    if (!path_.isEmpty() && !finished_) {
        const auto close = QDBusMessage::createMethodCall(
            service, path_, requestInterface, QStringLiteral("Close"));
        bus_.send(close);
    }
    unsubscribe();
}

void Request::start(const QString &interface, const QString &method, QVariantList arguments,
                    QVariantMap options)
{
    if (!bus_.isConnected()) {
        fail();
        return;
    }

    QString sender = bus_.baseService().mid(1);
    sender.replace(u'.', u'_');
    const auto handle = token();
    const auto expectedPath = path + QStringLiteral("/request/") + sender + u'/' + handle;
    if (!subscribe(expectedPath)) {
        fail();
        return;
    }

    auto message = QDBusMessage::createMethodCall(service, path, interface, method);
    options.insert(QStringLiteral("handle_token"), handle);
    arguments.append(options);
    message.setArguments(arguments);
    auto *call = new QDBusPendingCallWatcher(bus_.asyncCall(message), this);
    connect(call, &QDBusPendingCallWatcher::finished, this, [this, call] {
        const QDBusPendingReply<QDBusObjectPath> reply = *call;
        call->deleteLater();
        if (finished_)
            return;
        if (reply.isError() || reply.value().path().isEmpty()) {
            fail();
        } else if (reply.value().path() != path_) {
            unsubscribe();
            if (!subscribe(reply.value().path()))
                fail();
        }
    });
    QTimer::singleShot(120'000, this, [this] {
        if (!finished_)
            fail();
    });
}

bool Request::subscribe(const QString &signalPath)
{
    if (!bus_.connect(service, signalPath, requestInterface, responseSignal,
                      this, SLOT(receiveResponse(uint,QVariantMap))))
        return false;
    path_ = signalPath;
    return true;
}

void Request::unsubscribe()
{
    if (!path_.isEmpty()) {
        bus_.disconnect(service, path_, requestInterface, responseSignal,
                        this, SLOT(receiveResponse(uint,QVariantMap)));
        path_.clear();
    }
}

void Request::receiveResponse(uint code, const QVariantMap &results)
{
    if (finished_)
        return;
    finished_ = true;
    unsubscribe();
    deleteLater();
    emit responded(code, results);
}

void Request::fail()
{
    if (!path_.isEmpty()) {
        const auto close = QDBusMessage::createMethodCall(
            service, path_, requestInterface, QStringLiteral("Close"));
        bus_.send(close);
    }
    finished_ = true;
    unsubscribe();
    deleteLater();
    emit failed();
}
} // namespace Portal
