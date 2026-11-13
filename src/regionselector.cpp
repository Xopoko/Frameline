#include "regionselector.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegion>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#include <QtMath>
#include <QtNumeric>

#include <algorithm>
#include <utility>

#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
#include <qt_windows.h>
#include <qscreen_platform.h>
#define SCREENFRAME_NATIVE_MONITOR_GEOMETRY
#endif

#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace {

const QColor backdrop(12, 15, 25);
const QColor panel(22, 26, 43, 244);
const QColor divider(88, 88, 127);
const QColor accent(178, 154, 255);

#ifdef SCREENFRAME_NATIVE_MONITOR_GEOMETRY
QRectF nativeScreenGeometry(QScreen *screen)
{
    const auto *native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!native || !GetMonitorInfoW(native->handle(), &info))
        return {};
    const auto &r = info.rcMonitor;
    return QRectF(r.left, r.top, r.right - r.left, r.bottom - r.top);
}
#endif

#ifdef Q_OS_MACOS
void placeAboveMenuBar(WId windowId)
{
    auto view = reinterpret_cast<id>(windowId);
    const auto window = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(
        view, sel_registerName("window"));
    if (window) {
        reinterpret_cast<void (*)(id, SEL, long)>(objc_msgSend)(
            window, sel_registerName("setLevel:"), CGWindowLevelForKey(kCGScreenSaverWindowLevelKey));
    }
}
#endif

QPointF mapPoint(const QPointF &point, const QRectF &from, const QRectF &to)
{
    return {to.x() + (point.x() - from.x()) * to.width() / from.width(),
            to.y() + (point.y() - from.y()) * to.height() / from.height()};
}

QRectF mapRect(const QRectF &rect, const QRectF &from, const QRectF &to)
{
    return {mapPoint(rect.topLeft(), from, to), mapPoint(rect.bottomRight(), from, to)};
}

QRect pixelRect(const QRectF &rect)
{
    return {qRound(rect.x()), qRound(rect.y()),
            qRound(rect.right()) - qRound(rect.x()), qRound(rect.bottom()) - qRound(rect.y())};
}

class Overlay;

class Session final : public QObject
{
public:
    explicit Session(std::function<void(QImage)> done)
        : QObject(QCoreApplication::instance()), done_(std::move(done)) {}

    void show(const QList<CaptureFrame> &frames);
    void begin(Overlay *overlay, const QMouseEvent &event);
    void move(Overlay *overlay, const QMouseEvent &event);
    void end(Overlay *overlay, const QMouseEvent &event);
    void finish(QImage image = {});

    QRectF selection() const { return dragging_ ? QRectF(anchor_, cursor_).normalized() : QRectF(); }
    bool dragging() const { return dragging_; }

private:
    friend class Overlay;

    struct Crop {
        const QImage *image;
        QRectF source;
        QRectF layout;
    };

    QList<Crop> crops() const;
    QRectF outputBounds(const QList<Crop> &parts) const;
    QSize outputSize() const;
    QImage cropImage() const;
    QPointF cursorPoint(const Overlay *overlay, const QMouseEvent &event) const;
    void refreshOverlays();

    std::function<void(QImage)> done_;
    QList<Overlay *> overlays_;
    QRectF desktop_;
    bool nativeCoordinates_ = false;
    bool dragging_ = false;
    bool finished_ = false;
    QPointF anchor_;
    QPointF cursor_;
};

class Overlay final : public QWidget
{
public:
    Overlay(Session *session, QImage image, QScreen *screen, const QRectF &layout)
        : QWidget(nullptr, (screen ? Qt::Tool : Qt::Window) | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
          session_(session), image_(std::move(image)), layout_(layout), fit_(screen == nullptr)
    {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setCursor(Qt::CrossCursor);
        setFocusPolicy(Qt::StrongFocus);
        setWindowTitle(QStringLiteral("ScreenFrame — Выбор области"));
        setAccessibleDescription(QStringLiteral("Выделите прямоугольную область. Escape — отмена."));

        cancel_ = new QPushButton(QStringLiteral("Отмена  Esc"), this);
        cancel_->setAccessibleName(QStringLiteral("Отменить выбор области"));
        cancel_->setCursor(Qt::PointingHandCursor);
        cancel_->installEventFilter(this);
        cancel_->setStyleSheet(QStringLiteral(
            "QPushButton { background: #252a42; color: #ebe8ff; border: 1px solid #514c72;"
            " border-radius: 7px; padding: 6px 11px; }"
            "QPushButton:hover, QPushButton:focus { background: #373052; border-color: #b29aff; }"));
        connect(cancel_, &QPushButton::clicked, session_, [session] { session->finish(); });

        if (screen) {
            setGeometry(screen->geometry());
            winId();
            windowHandle()->setScreen(screen);
            setGeometry(screen->geometry());
        }
    }

    const QImage &image() const { return image_; }
    QRectF layout() const { return layout_; }

    QRectF viewport() const
    {
        if (!fit_)
            return QRectF(rect());

        const QSizeF fitted = QSizeF(image_.size()).scaled(QSizeF(size()), Qt::KeepAspectRatio);
        return QRectF(QPointF((width() - fitted.width()) / 2.0, (height() - fitted.height()) / 2.0),
                      fitted);
    }

    bool fitsImage() const { return fit_; }

    void showFrame()
    {
        if (fit_) {
            showFullScreen();
        } else {
            show();
#ifdef Q_OS_MACOS
            placeAboveMenuBar(winId());
#endif
        }
    }

    void updateSelection(bool dragging, const QRectF &area, const QString &measurement)
    {
        const QRectF photo = viewport();
        const QRectF selection = area.isEmpty() || photo.isEmpty() ? QRectF() : mapRect(area, layout_, photo);
        const QRectF visible = selection.intersected(photo);
        const QRectF badge = visible.isEmpty() || measurement.isEmpty() ? QRectF() : measurementRect(visible, measurement);

        QRegion damage = QRegion(visible_.toAlignedRect()).xored(QRegion(visible.toAlignedRect()));
        if (selection != selection_) {
            damage += outline(selection_, photo);
            damage += outline(selection, photo);
        }
        if (badge != badge_ || measurement != measurement_) {
            if (!badge_.isEmpty())
                damage += badge_.toAlignedRect().adjusted(-2, -2, 2, 2);
            if (!badge.isEmpty())
                damage += badge.toAlignedRect().adjusted(-2, -2, 2, 2);
        }
        if (dragging != dragging_) {
            dragging_ = dragging;
            cancel_->setVisible(!dragging);
            damage += instructionsRect().toAlignedRect().adjusted(-2, -2, 2, 2);
        }

        selection_ = selection;
        visible_ = visible;
        badge_ = badge;
        measurement_ = measurement;
        if (!damage.isEmpty())
            update(damage);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == cancel_ && event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            session_->finish();
            return true;
        }
        return QWidget::eventFilter(object, event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
            session_->begin(this, *event);
        else if (event->button() == Qt::RightButton)
            session_->finish();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (session_->dragging())
            session_->move(this, *event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && session_->dragging())
            session_->end(this, *event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            event->accept();
            session_->finish();
        } else {
            QWidget::keyPressEvent(event);
        }
    }

    void closeEvent(QCloseEvent *event) override
    {
        event->accept();
        session_->finish();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        cancel_->adjustSize();
        cancel_->move(std::max(8, width() - cancel_->width() - 24), 24);
        if (dragging_)
            session_->refreshOverlays();
    }

    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.fillRect(event->rect(), backdrop);
        const QRectF photo = viewport();
        if (photo.isEmpty())
            return;

        drawPhoto(painter, photo);

        painter.save();
        const QRegion dark = QRegion(photo.toAlignedRect()).subtracted(QRegion(visible_.toAlignedRect()));
        painter.setClipRegion(dark);
        painter.fillRect(photo, QColor(10, 13, 24, 148));
        painter.restore();

        if (!dragging_) {
            drawInstructions(painter);
        } else if (!visible_.isEmpty()) {
            painter.save();
            painter.setClipRect(photo);
            painter.setPen(QPen(QColor(17, 18, 35), 3));
            painter.drawRect(selection_);
            painter.setPen(QPen(accent, 1));
            painter.drawRect(selection_);
            painter.restore();

            if (!badge_.isEmpty())
                drawMeasurement(painter);
        }
    }

private:
    void drawPhoto(QPainter &painter, const QRectF &photo)
    {
        const qreal dpr = devicePixelRatioF();
        const QSizeF pixels = photo.size() * dpr;
        if (qFuzzyCompare(pixels.width(), qreal(image_.width()))
            && qFuzzyCompare(pixels.height(), qreal(image_.height()))
            && qFuzzyIsNull(photo.x() * dpr - qRound(photo.x() * dpr))
            && qFuzzyIsNull(photo.y() * dpr - qRound(photo.y() * dpr))) {
            painter.drawImage(photo, image_);
            return;
        }

        // Rasterize a scaled frame once, using the complete viewport so rounding does not
        // depend on which part of the backing store was damaged. Keep the original for crop.
        if (preview_.isNull() || photo != previewPhoto_ || dpr != previewDpr_) {
            const QRect pixelBounds = QRectF(photo.topLeft() * dpr, pixels).toAlignedRect();
            preview_ = QImage(pixelBounds.size(), QImage::Format_ARGB32_Premultiplied);
            preview_.setDevicePixelRatio(dpr);
            previewOrigin_ = QPointF(pixelBounds.topLeft()) / dpr;
            previewPhoto_ = photo;
            previewDpr_ = dpr;
            if (!preview_.isNull()) {
                preview_.fill(Qt::transparent);
                QPainter raster(&preview_);
                raster.setRenderHint(QPainter::SmoothPixmapTransform);
                raster.drawImage(photo.translated(-previewOrigin_), image_);
            }
        }

        if (!preview_.isNull()) {
            painter.drawImage(previewOrigin_, preview_);
        } else {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(photo, image_);
        }
    }

    static QRegion outline(const QRectF &selection, const QRectF &photo)
    {
        if (selection.isEmpty())
            return {};
        // Include the full stroke, but exclude the unchanged interior from the damage.
        const QRect outer = selection.toAlignedRect().adjusted(-3, -3, 3, 3);
        const QRect inner = selection.toAlignedRect().adjusted(3, 3, -3, -3);
        return QRegion(outer.intersected(photo.toAlignedRect())).subtracted(QRegion(inner));
    }

    QFont instructionTitleFont() const
    {
        QFont titleFont = font();
        titleFont.setBold(true);
        titleFont.setPointSizeF(10.5);
        return titleFont;
    }

    QRectF instructionsRect() const
    {
        const int titleWidth = QFontMetrics(instructionTitleFont()).horizontalAdvance(title_);
        const int helpWidth = QFontMetrics(font()).horizontalAdvance(help_);
        return QRectF(24, 24, std::max(titleWidth, helpWidth) + 57, 57);
    }

    void drawInstructions(QPainter &painter) const
    {
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(panel);
        painter.setPen(divider);
        painter.drawRoundedRect(instructionsRect(), 9, 9);

        painter.setPen(QPen(accent, 1.4));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(43, 45, 16, 16));
        painter.drawLine(QPointF(36, 41), QPointF(46, 41));
        painter.drawLine(QPointF(41, 36), QPointF(41, 46));

        painter.setPen(QColor(245, 242, 255));
        painter.setFont(instructionTitleFont());
        painter.drawText(QPointF(72, 47), title_);
        painter.setFont(font());
        painter.setPen(QColor(191, 193, 216));
        painter.drawText(QPointF(72, 66), help_);
    }

    QFont measurementFont() const
    {
        QFont labelFont = font();
        labelFont.setBold(true);
        return labelFont;
    }

    QRectF measurementRect(const QRectF &visible, const QString &label) const
    {
        const int badgeWidth = QFontMetrics(measurementFont()).horizontalAdvance(label) + 18;
        const qreal maxX = std::max(7, width() - badgeWidth - 7);
        const qreal x = std::clamp(visible.right() - badgeWidth, 7.0, maxX);
        const qreal y = visible.top() >= 38 ? visible.top() - 34 : visible.top() + 7;
        return QRectF(x, y, badgeWidth, 27);
    }

    void drawMeasurement(QPainter &painter) const
    {
        painter.setFont(measurementFont());
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(panel);
        painter.setPen(accent);
        painter.drawRoundedRect(badge_, 6, 6);
        painter.setPen(QColor(245, 242, 255));
        painter.drawText(badge_, Qt::AlignCenter, measurement_);
    }

    Session *session_;
    const QImage image_;
    const QRectF layout_;
    const bool fit_;
    const QString title_ = QStringLiteral("Выберите область");
    const QString help_ = QStringLiteral("Нажмите и перетащите курсор");
    bool dragging_ = false;
    QRectF selection_;
    QRectF visible_;
    QRectF badge_;
    QString measurement_;
    QImage preview_;
    QRectF previewPhoto_;
    QPointF previewOrigin_;
    qreal previewDpr_ = 0;
    QPushButton *cancel_ = nullptr;
};

void Session::show(const QList<CaptureFrame> &frames)
{
#ifdef SCREENFRAME_NATIVE_MONITOR_GEOMETRY
    nativeCoordinates_ = std::all_of(frames.cbegin(), frames.cend(), [](const CaptureFrame &frame) {
        return frame.image.isNull() || (frame.screen && !nativeScreenGeometry(frame.screen).isEmpty());
    });
#endif

    Overlay *active = nullptr;
    const auto cursorScreen = QGuiApplication::screenAt(QCursor::pos());
    for (const auto &frame : frames) {
        if (frame.image.isNull())
            continue;

        QRectF layout = frame.screen ? QRectF(frame.screen->geometry()) : QRectF(frame.image.rect());
#ifdef SCREENFRAME_NATIVE_MONITOR_GEOMETRY
        // Qt leaves gaps in the virtual desktop geometry between displays with different scale factors.
        if (nativeCoordinates_)
            layout = nativeScreenGeometry(frame.screen);
#endif
        if (layout.isEmpty())
            continue;

        auto *overlay = new Overlay(this, frame.image, frame.screen, layout);
        overlays_.append(overlay);
        desktop_ = desktop_.united(layout);
        if (frame.screen == cursorScreen || !active)
            active = overlay;
    }

    if (!active) {
        finish();
        return;
    }

    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { finish(); });
    for (auto *overlay : overlays_)
        overlay->showFrame();
    active->raise();
    active->activateWindow();
    active->setFocus(Qt::ActiveWindowFocusReason);
}

void Session::begin(Overlay *overlay, const QMouseEvent &event)
{
    if (finished_ || dragging_ || !overlay->viewport().contains(event.position()))
        return;

    anchor_ = cursorPoint(overlay, event);
    cursor_ = anchor_;
    dragging_ = true;
    refreshOverlays();
}

void Session::move(Overlay *overlay, const QMouseEvent &event)
{
    const QPointF cursor = cursorPoint(overlay, event);
    if (cursor != cursor_) {
        cursor_ = cursor;
        refreshOverlays();
    }
}

void Session::end(Overlay *overlay, const QMouseEvent &event)
{
    cursor_ = cursorPoint(overlay, event);
    QImage image = cropImage();
    if (!image.isNull()) {
        finish(std::move(image));
        return;
    }

    dragging_ = false;
    refreshOverlays();
}

void Session::finish(QImage image)
{
    if (finished_)
        return;

    finished_ = true;
    for (auto *overlay : overlays_) {
        overlay->hide();
        overlay->deleteLater();
    }
    overlays_.clear();

    // The completion callback can open the editor or quit the application.
    QTimer::singleShot(0, this, [this, image = std::move(image)]() mutable {
        auto done = std::move(done_);
        deleteLater();
        done(std::move(image));
    });
}

QPointF Session::cursorPoint(const Overlay *overlay, const QMouseEvent &event) const
{
    QPointF point = overlay->fitsImage()
        ? mapPoint(event.position(), overlay->viewport(), overlay->layout())
        : event.globalPosition();
#ifdef SCREENFRAME_NATIVE_MONITOR_GEOMETRY
    POINT native;
    if (nativeCoordinates_ && GetCursorPos(&native))
        point = QPointF(native.x, native.y);
#endif
    // The pointer addresses pixel centers. Allow the outermost screen pixels to be included
    // when the drag ends at a desktop edge.
    if (!overlay->fitsImage()) {
        if (point.x() >= desktop_.right() - 1)
            point.setX(desktop_.right());
        if (point.y() >= desktop_.bottom() - 1)
            point.setY(desktop_.bottom());
    }
    return {std::clamp(point.x(), desktop_.left(), desktop_.right()),
            std::clamp(point.y(), desktop_.top(), desktop_.bottom())};
}

void Session::refreshOverlays()
{
    const QRectF area = selection();
    const auto current = std::find_if(overlays_.cbegin(), overlays_.cend(), [this](const Overlay *item) {
        return item->layout().contains(cursor_);
    });
    const QSize pixels = area.isEmpty() ? QSize() : outputSize();
    const QString measurement = pixels.isEmpty()
        ? QString() : QStringLiteral("%1 × %2 px").arg(pixels.width()).arg(pixels.height());

    for (auto *overlay : overlays_) {
        const QRectF layout = overlay->layout();
        const bool showMeasurement = layout.contains(cursor_) || (current == overlays_.cend() && layout.contains(anchor_));
        overlay->updateSelection(dragging_, area, showMeasurement ? measurement : QString());
    }
}

QList<Session::Crop> Session::crops() const
{
    QList<Crop> parts;
    const QRectF area = selection();
    for (const auto *overlay : overlays_) {
        const QRectF visible = overlay->layout().intersected(area);
        if (!visible.isEmpty()) {
            const auto &image = overlay->image();
            const QRectF source = mapRect(visible, overlay->layout(), QRectF(image.rect()));
            if (!pixelRect(source).isEmpty())
                parts.append({&image, source, overlay->layout()});
        }
    }
    return parts;
}

QRectF Session::outputBounds(const QList<Crop> &parts) const
{
    // Native coordinates on Windows already use a common physical pixel grid. On other
    // systems, use the highest density of the intersecting screens for cross-screen crops.
    qreal scale = 1;
    if (!nativeCoordinates_) {
        for (const auto &part : parts)
            scale = std::max(scale, part.image->width() / part.layout.width());
    }
    const QRectF area = selection();
    return {area.topLeft() * scale, area.size() * scale};
}

QSize Session::outputSize() const
{
    const auto parts = crops();
    if (parts.isEmpty())
        return {};
    if (parts.size() == 1)
        return pixelRect(parts.constFirst().source).size();
    return pixelRect(outputBounds(parts)).size();
}

QImage Session::cropImage() const
{
    const auto parts = crops();
    if (parts.isEmpty())
        return {};
    if (parts.size() == 1) {
        const auto &part = parts.constFirst();
        QImage image = part.image->copy(pixelRect(part.source));
        image.setDevicePixelRatio(1);
        return image;
    }

    const QRectF area = selection();
    const QRectF targetBounds = outputBounds(parts);
    const QRect target = pixelRect(targetBounds);
    QImage result(target.size(), QImage::Format_ARGB32_Premultiplied);
    if (result.isNull())
        return {};
    result.fill(Qt::transparent);

    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const auto &part : parts) {
        const QRectF layoutPart = part.layout.intersected(area);
        const QRect destination = pixelRect(mapRect(layoutPart, area, targetBounds)).translated(-target.topLeft());
        painter.drawImage(destination, *part.image, pixelRect(part.source));
    }
    return result;
}

} // namespace

void RegionSelector::start(const QList<CaptureFrame> &frames, std::function<void(QImage)> done)
{
    if (done)
        (new Session(std::move(done)))->show(frames);
}
