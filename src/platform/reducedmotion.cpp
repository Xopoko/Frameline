#include "reducedmotion.h"

#include <QApplication>
#include <QStyle>

#ifdef Q_OS_WIN
#  include <qt_windows.h>
#elif defined(Q_OS_LINUX)
#  include "portal_request.h"
#  include <QDBusMessage>
#  include <QDBusVariant>
#  include <QDeadlineTimer>

namespace {
QVariant readSetting(const QDBusConnection &bus, const QString &group, const QString &key,
                     const QDeadlineTimer &deadline)
{
    if (!bus.isConnected() || deadline.hasExpired())
        return {};

    auto request = QDBusMessage::createMethodCall(Portal::service, Portal::path,
                                                  QStringLiteral("org.freedesktop.portal.Settings"),
                                                  QStringLiteral("Read"));
    request.setArguments({group, key});
    const int timeout = qMax(1, int(deadline.remainingTime()));
    const auto reply = bus.call(request, QDBus::Block, timeout);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1)
        return {};

    // Older Settings portals only offer Read, which wraps the reply in two variants.
    QVariant value = reply.arguments().first();
    while (value.typeId() == qMetaTypeId<QDBusVariant>())
        value = value.value<QDBusVariant>().variant();
    return value;
}
} // namespace
#endif

bool prefersReducedMotion()
{
#ifdef Q_OS_WIN
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
        return !enabled;
#elif defined(Q_OS_LINUX)
    const QDeadlineTimer deadline(120);
    const auto bus = QDBusConnection::sessionBus();
    const auto reducedMotion = readSetting(bus, QStringLiteral("org.freedesktop.appearance"),
                                           QStringLiteral("reduced-motion"), deadline);
    if (reducedMotion.typeId() == QMetaType::UInt)
        return reducedMotion.toUInt() == 1;

    // GNOME and GTK backends on older portal versions expose the desktop preference directly.
    const auto animations = readSetting(bus, QStringLiteral("org.gnome.desktop.interface"),
                                         QStringLiteral("enable-animations"), deadline);
    if (animations.typeId() == QMetaType::Bool)
        return !animations.toBool();
#endif
    return qobject_cast<QApplication *>(QCoreApplication::instance())
        && QApplication::style()->styleHint(QStyle::SH_Widget_Animation_Duration) == 0;
}
