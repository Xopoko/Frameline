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
#elif defined(Q_OS_WIN)
#include <qt_windows.h>

#include <memory>
#include <type_traits>

namespace {
QImage grabWindowsScreen(QScreen *screen)
{
    // On Windows Qt retains the monitor's native origin in its logical geometry.
    const auto origin = screen->geometry().topLeft();
    const auto monitor = MonitorFromPoint({origin.x(), origin.y()}, MONITOR_DEFAULTTONULL);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfo(monitor, &info)
        || info.rcMonitor.left != origin.x() || info.rcMonitor.top != origin.y())
        return {};

    const auto desktopWindow = GetDesktopWindow();
    const auto desktop = GetDC(desktopWindow);
    if (!desktop)
        return {};
    const auto releaseDesktop = [desktopWindow](HDC dc) { ReleaseDC(desktopWindow, dc); };
    const std::unique_ptr<std::remove_pointer_t<HDC>, decltype(releaseDesktop)> source(
        desktop, releaseDesktop);
    const std::unique_ptr<std::remove_pointer_t<HDC>, decltype(&DeleteDC)> destination(
        CreateCompatibleDC(source.get()), DeleteDC);
    if (!destination)
        return {};

    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int height = info.rcMonitor.bottom - info.rcMonitor.top;
    BITMAPINFO format{};
    format.bmiHeader.biSize = sizeof(format.bmiHeader);
    format.bmiHeader.biWidth = width;
    format.bmiHeader.biHeight = -height; // QImage and the bitmap both store the top row first.
    format.bmiHeader.biPlanes = 1;
    format.bmiHeader.biBitCount = 32;
    format.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, decltype(&DeleteObject)> bitmap(
        CreateDIBSection(source.get(), &format, DIB_RGB_COLORS, &bits, nullptr, 0), DeleteObject);
    if (!bitmap || !bits)
        return {};

    const auto previous = SelectObject(destination.get(), bitmap.get());
    if (!previous || previous == HGDI_ERROR)
        return {};
    const bool copied = BitBlt(destination.get(), 0, 0, width, height, source.get(),
                               origin.x(), origin.y(), SRCCOPY | CAPTUREBLT);
    const bool flushed = GdiFlush();
    SelectObject(destination.get(), previous);
    if (!copied || !flushed)
        return {};

    // A GDI BGRX bitmap leaves the top byte unspecified. Qt RGB32 requires opaque pixels.
    auto *pixels = static_cast<QRgb *>(bits);
    const auto count = qsizetype(width) * height;
    for (qsizetype index = 0; index < count; ++index)
        pixels[index] |= 0xff000000u;

    QImage image(static_cast<uchar *>(bits), width, height, qsizetype(width) * sizeof(QRgb),
                 QImage::Format_RGB32, [](void *handle) { DeleteObject(handle); }, bitmap.get());
    if (!image.isNull())
        bitmap.release();
    return image;
}
} // namespace
#endif

bool ScreenCapture::usesScreenshotPortal()
{
#if defined(Q_OS_LINUX)
    return Portal::isWayland();
#else
    return false;
#endif
}

void ScreenCapture::capture(bool region, QWidget *portalParent)
{
#if defined(Q_OS_LINUX)
    if (usesScreenshotPortal()) {
        auto *request = new Portal::Request(this);
        const auto message = QStringLiteral("Wayland did not provide a screenshot. Check screen capture permissions in system settings.");
        const auto responseFailure = portalParent
            ? QStringLiteral("Wayland denied screen capture. Allow FreezeCut to capture the screen in system settings and try again.")
            : QStringLiteral("Wayland denied screen capture. On the first run, try again from the active FreezeCut window.");
        connect(request, &Portal::Request::failed, this, [this, message] { emit failed(message); });
        connect(request, &Portal::Request::responded, this,
                [this, responseFailure](uint code, const QVariantMap &results) {
            if (code == 1) {
                emit cancelled();
                return;
            }
            if (code == 2) {
                emit portalPermissionRequired(responseFailure);
                return;
            }
            if (code != 0) {
                emit failed(QStringLiteral("Could not get a screenshot from Wayland."));
                return;
            }
            const QUrl uri(results.value(QStringLiteral("uri")).toString());
            QImage image;
            if (uri.isLocalFile())
                image.load(uri.toLocalFile());
            if (image.isNull()) {
                emit failed(QStringLiteral("Could not open the screenshot from Wayland."));
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

    auto *activeScreen = QGuiApplication::screenAt(QCursor::pos());
    if (!activeScreen)
        activeScreen = QGuiApplication::primaryScreen();
    const auto screens = region ? QGuiApplication::screens() : QList<QScreen *>{activeScreen};

#if defined(Q_OS_MACOS)
    captureMacScreens(this, screens);
#else
    QList<CaptureFrame> frames;
    for (auto *screen : screens) {
        if (!screen)
            continue;
        QImage image;
#if defined(Q_OS_WIN)
        image = grabWindowsScreen(screen);
#endif
        if (image.isNull())
            image = screen->grabWindow(0).toImage();
        if (!image.isNull()) {
            image.setDevicePixelRatio(1);
            frames.append({image, screen});
        }
    }
    if (frames.isEmpty())
        emit failed(QStringLiteral("Could not capture the screen."));
    else
        emit captured(frames);
#endif
}
