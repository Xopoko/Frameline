#include "capture.h"

#include <QCursor>
#include <QGuiApplication>
#include <QPixmap>

#if defined(Q_OS_LINUX)
#include "platform/portal_request.h"
#include <QUrl>
#include <QWidget>
#elif defined(Q_OS_MACOS)
#include "platform/macos_capture.h"
#endif

bool ScreenCapture::usesScreenshotPortal()
{
#if defined(Q_OS_LINUX)
    return Portal::isWayland();
#else
    return false;
#endif
}

void ScreenCapture::capture(bool region, QWidget* portalParent)
{
#if defined(Q_OS_LINUX)
    if (usesScreenshotPortal()) {
        auto* request = new Portal::Request(this);
        const auto message = tr("Wayland не предоставил снимок. Проверьте разрешение "
                                "на снимки экрана в настройках системы.");
        const auto responseFailure = portalParent
            ? tr("Wayland не разрешил захват экрана. Разрешите приложению делать снимки "
                 "в настройках системы и повторите попытку.")
            : tr("Wayland не разрешил захват экрана. При первом запуске попробуйте ещё раз "
                 "из активного окна ScreenFrame. Оно может попасть в первый снимок.");
        connect(request, &Portal::Request::failed, this, [this, message] { emit failed(message); });
        connect(request, &Portal::Request::responded, this,
                [this, responseFailure](uint code, const QVariantMap& results) {
            if (code == 1) {
                emit cancelled();
                return;
            }
            if (code == 2) {
                emit portalPermissionRequired(responseFailure);
                return;
            }
            if (code != 0) {
                emit failed(tr("Не удалось получить снимок от Wayland."));
                return;
            }
            const QUrl uri(results.value(QStringLiteral("uri")).toString());
            QImage image;
            if (uri.isLocalFile())
                image.load(uri.toLocalFile());
            if (image.isNull()) {
                emit failed(tr("Не удалось открыть снимок, полученный от Wayland."));
                return;
            }
            image.setDevicePixelRatio(1);
            emit captured({{image, nullptr}});
        });
        QString windowId;
        if (portalParent && QGuiApplication::platformName() == QStringLiteral("xcb"))
            windowId = QStringLiteral("x11:") + QString::number(portalParent->winId(), 16);
        request->start(QStringLiteral("org.freedesktop.portal.Screenshot"),
                       QStringLiteral("Screenshot"), {windowId},
                       {{QStringLiteral("interactive"), false}});
        return;
    }
#else
    Q_UNUSED(portalParent);
#endif

    auto* activeScreen = QGuiApplication::screenAt(QCursor::pos());
    if (!activeScreen)
        activeScreen = QGuiApplication::primaryScreen();
    const auto screens = region ? QGuiApplication::screens() : QList<QScreen*>{activeScreen};

#if defined(Q_OS_MACOS)
    captureMacScreens(this, screens);
#else
    QList<CaptureFrame> frames;
    for (auto* screen : screens) {
        if (!screen)
            continue;
        auto image = screen->grabWindow(0).toImage();
        if (!image.isNull()) {
            image.setDevicePixelRatio(1);
            frames.append({image, screen});
        }
    }
    if (frames.isEmpty())
        emit failed(tr("Не удалось сделать снимок экрана."));
    else
        emit captured(frames);
#endif
}
