#include "macos_capture.h"

#include <QColorSpace>
#include <QMetaObject>

#import <ApplicationServices/ApplicationServices.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

namespace {
struct Display {
    CGDirectDisplayID id;
    QPointer<QScreen> screen;
};

QImage toImage(CGImageRef source)
{
    if (!source)
        return {};
    const auto width = CGImageGetWidth(source);
    const auto height = CGImageGetHeight(source);
    QImage image(int(width), int(height), QImage::Format_RGBX8888);
    if (image.isNull())
        return {};
    const auto colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const auto context = CGBitmapContextCreate(image.bits(), width, height, 8,
                                               image.bytesPerLine(), colorSpace,
                                               CGBitmapInfo(kCGImageAlphaNoneSkipLast) | kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(colorSpace);
    if (!context)
        return {};
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), source);
    CGContextRelease(context);
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}

// The Qt screen and Core Graphics display use the same top-left coordinate system.
QList<Display> matchDisplays(const QList<QScreen *> &screens)
{
    uint32_t count = 0;
    if (CGGetActiveDisplayList(0, nullptr, &count) != kCGErrorSuccess || !count)
        return {};
    QList<CGDirectDisplayID> ids(count);
    if (CGGetActiveDisplayList(count, ids.data(), &count) != kCGErrorSuccess)
        return {};
    ids.resize(count);

    QList<Display> displays;
    for (auto *screen : screens) {
        if (!screen)
            continue;
        for (auto id : ids) {
            const auto bounds = CGDisplayBounds(id);
            const QRect geometry(qRound(bounds.origin.x), qRound(bounds.origin.y),
                                 qRound(bounds.size.width), qRound(bounds.size.height));
            if (geometry == screen->geometry()) {
                displays.append({id, screen});
                break;
            }
        }
    }
    return displays;
}
} // namespace

void captureMacScreens(ScreenCapture *capture, const QList<QScreen *> &screens)
{
    if (!CGPreflightScreenCaptureAccess() && !CGRequestScreenCaptureAccess()) {
        emit capture->failed(QStringLiteral(
            "Allow FreezeCut in System Settings → Privacy & Security → Screen & System Audio Recording, "
            "then restart the app."));
        return;
    }

    const auto displays = matchDisplays(screens);
    const auto application = QCoreApplication::instance();
    const auto processId = QCoreApplication::applicationPid();
    const QPointer<ScreenCapture> owner(capture);
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent *content, NSError *) {
        NSArray<SCRunningApplication *> *excludedApplications = @[];
        for (SCRunningApplication *app in content.applications) {
            if (app.processID == processId) {
                excludedApplications = @[app];
                break;
            }
        }
        const auto queue = dispatch_group_create();
        const auto resultsQueue = dispatch_queue_create("freezecut.capture", DISPATCH_QUEUE_SERIAL);
        __block QList<CaptureFrame> frames;
        for (const auto &display : displays) {
            for (SCDisplay *source in content.displays) {
                if (source.displayID != display.id)
                    continue;
                const auto filter = [[SCContentFilter alloc] initWithDisplay:source
                                                     excludingApplications:excludedApplications
                                                          exceptingWindows:@[]];
                const auto config = [SCStreamConfiguration new];
                config.width = qRound(filter.contentRect.size.width * filter.pointPixelScale);
                config.height = qRound(filter.contentRect.size.height * filter.pointPixelScale);
                config.showsCursor = NO;
                const auto screen = display.screen;
                dispatch_group_enter(queue);
                [SCScreenshotManager captureImageWithFilter:filter configuration:config
                                           completionHandler:^(CGImageRef image, NSError *) {
                    const auto pixels = toImage(image);
                    if (!pixels.isNull()) {
                        dispatch_sync(resultsQueue, ^{
                            frames.append({pixels, screen});
                        });
                    }
                    dispatch_group_leave(queue);
                }];
                break;
            }
        }
        dispatch_group_notify(queue, dispatch_get_main_queue(), ^{
            const auto capturedFrames = frames;
            QMetaObject::invokeMethod(application, [owner, capturedFrames] {
                if (!owner)
                    return;
                if (capturedFrames.isEmpty())
                    emit owner->failed(QStringLiteral("Could not capture the screen on macOS."));
                else
                    emit owner->captured(capturedFrames);
            }, Qt::QueuedConnection);
        });
    }];
}
