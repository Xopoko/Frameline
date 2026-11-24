#include "regionselector.h"

#include "theme.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QFontMetrics>
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
#define FREEZECUT_NATIVE_MONITOR_GEOMETRY
#endif

#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace {

// The overlay sits on arbitrary screen content, so it keeps one dark treatment in both themes.
const QColor backdrop(19, 19, 22);
const QColor scrim(18, 18, 22, 138);
const QColor panel(32, 32, 36);
const QColor panelBorder(83, 83, 91);
const QColor panelText(245, 244, 242);
const QColor panelMuted(200, 199, 204);
const QColor accent = theme::overlayAccent;

constexpr int panelMargin = 8;
constexpr int panelHeight = 40;
constexpr int panelPadding = 4;
constexpr int instructionInset = 12;
constexpr int iconSize = 16;
constexpr int iconSpacing = 8;
constexpr int actionSpacing = 6;

void drawPanel(QPainter &painter, const QRectF &area, qreal radius)
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(panelBorder, 1));
    painter.setBrush(panel);
    painter.drawRoundedRect(area.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

#ifdef FREEZECUT_NATIVE_MONITOR_GEOMETRY
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
    explicit Session(RegionSelector::Completion done)
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
    void dismissOverlays();

    RegionSelector::Completion done_;
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
        setMouseTracking(true);
        setCursor(Qt::CrossCursor);
        setFocusPolicy(Qt::StrongFocus);
        setWindowTitle(QStringLiteral("FreezeCut — Select region"));
        setAccessibleDescription(QStringLiteral("Drag to capture a region. Press Esc or right-click to cancel."));

        cancel_ = new QPushButton(QStringLiteral("Cancel  ·  Esc"), this);
        cancel_->setAccessibleName(QStringLiteral("Cancel region selection"));
        cancel_->setToolTip(QStringLiteral("Press Esc or right-click to cancel"));
        cancel_->setCursor(Qt::PointingHandCursor);
        cancel_->setFont(theme::font(13, theme::Weight::Medium));
        cancel_->installEventFilter(this);
        cancel_->setStyleSheet(QStringLiteral(
            "QPushButton { background: transparent; color: %1; border: 1px solid transparent;"
            " border-radius: 5px; padding: 0 10px; outline: 0; }"
            "QPushButton:hover { background: #39393e; color: %2; }"
            "QPushButton:pressed { background: #484850; }"
            "QPushButton:focus { border-color: %3; }")
            .arg(panelMuted.name(), panelText.name(), accent.name()));
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
            damage += instructions_;
            damage += guides(hover_);
            if (dragging)
                hover_ = QPointF(-1, -1);
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
        if (session_->dragging()) {
            session_->move(this, *event);
            return;
        }
        moveGuides(viewport().contains(event->position()) ? event->position() : QPointF(-1, -1));
    }

    void leaveEvent(QEvent *event) override
    {
        moveGuides(QPointF(-1, -1));
        QWidget::leaveEvent(event);
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
        layoutInstructions();
        hover_ = QPointF(-1, -1);
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
        painter.fillRect(photo, scrim);
        painter.restore();

        if (!dragging_) {
            drawGuides(painter, photo);
            drawInstructions(painter);
        } else if (!visible_.isEmpty()) {
            painter.save();
            painter.setClipRect(photo);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor(16, 16, 20, 210), 3));
            painter.drawRect(selection_);
            painter.setPen(QPen(accent, 1));
            painter.drawRect(selection_);
            const qreal corner = std::min({14.0, selection_.width() / 4, selection_.height() / 4});
            if (corner >= 3)
                theme::paintBrackets(painter, selection_.adjusted(-1, -1, 1, 1), corner, 2, accent);
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
        // Include the full stroke and the corner marks, but exclude the unchanged interior from the damage.
        const QRect outer = selection.toAlignedRect().adjusted(-6, -6, 6, 6);
        const QRect inner = selection.toAlignedRect().adjusted(6, 6, -6, -6);
        return QRegion(outer.intersected(photo.toAlignedRect())).subtracted(QRegion(inner));
    }

    void layoutInstructions()
    {
        const int titleWidth = QFontMetrics(instructionFont_).horizontalAdvance(instructionText_);
        const int cancelWidth = cancel_->sizeHint().width();
        const int fullWidth = instructionInset + iconSize + iconSpacing + titleWidth
            + instructionInset + 1 + actionSpacing + cancelWidth + panelPadding;
        const int available = std::max(0, width() - 2 * panelMargin);
        const int panelWidth = std::min(fullWidth, available);
        const int top = std::clamp(height() - panelHeight - panelMargin, 0, 20);
        instructions_ = QRect((width() - panelWidth) / 2, top, panelWidth, panelHeight);

        const int actionWidth = std::clamp(panelWidth - 2 * panelPadding, 0, cancelWidth);
        cancel_->setGeometry(instructions_.right() + 1 - panelPadding - actionWidth,
                             instructions_.top() + panelPadding,
                             actionWidth, panelHeight - 2 * panelPadding);
    }

    void drawInstructions(QPainter &painter) const
    {
        if (instructions_.isEmpty())
            return;

        const QRectF area(instructions_);
        const int divider = cancel_->x() - actionSpacing;
        const qreal titleLeft = area.left() + instructionInset + iconSize + iconSpacing;
        const int titleWidth = int(divider - instructionInset - titleLeft);
        painter.save();
        drawPanel(painter, area, 9);
        if (titleWidth > 0) {
            theme::paintBrackets(painter, QRectF(area.left() + instructionInset, area.center().y() - iconSize / 2.0,
                                                iconSize, iconSize), 5, 1.5, accent);
            painter.fillRect(QRectF(divider, area.top() + 10, 1, area.height() - 20), panelBorder);
            painter.setPen(panelText);
            painter.setFont(instructionFont_);
            const QFontMetrics metrics(instructionFont_);
            const QString title = metrics.horizontalAdvance(instructionText_) <= titleWidth
                ? instructionText_ : QStringLiteral("Drag");
            const QString text = metrics.elidedText(title, Qt::ElideRight, titleWidth);
            painter.drawText(QRectF(titleLeft, area.top(), titleWidth, area.height()), Qt::AlignVCenter, text);
        }
        painter.restore();
    }

    QRegion guides(const QPointF &point) const
    {
        if (point.x() < 0)
            return {};
        const QRect photo = viewport().toAlignedRect();
        const int x = qFloor(point.x());
        const int y = qFloor(point.y());
        return QRegion(QRect(x - 2, photo.top(), 5, photo.height())) + QRegion(QRect(photo.left(), y - 2, photo.width(), 5));
    }

    void moveGuides(const QPointF &point)
    {
        if (point == hover_)
            return;
        QRegion damage = guides(hover_);
        hover_ = point;
        damage += guides(hover_);
        if (!damage.isEmpty())
            update(damage);
    }

    void drawGuides(QPainter &painter, const QRectF &photo) const
    {
        if (hover_.x() < 0)
            return;
        const qreal x = qFloor(hover_.x()) + 0.5;
        const qreal y = qFloor(hover_.y()) + 0.5;
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(QPen(QColor(0, 0, 0, 70), 1));
        painter.drawLine(QPointF(x + 1, photo.top()), QPointF(x + 1, photo.bottom()));
        painter.drawLine(QPointF(photo.left(), y + 1), QPointF(photo.right(), y + 1));
        painter.setPen(QPen(QColor(245, 244, 242, 112), 1));
        painter.drawLine(QPointF(x, photo.top()), QPointF(x, photo.bottom()));
        painter.drawLine(QPointF(photo.left(), y), QPointF(photo.right(), y));
        painter.restore();
    }

    QFont measurementFont() const
    {
        return theme::font(12, theme::Weight::Semibold, true);
    }

    QRectF measurementRect(const QRectF &visible, const QString &label) const
    {
        constexpr qreal gap = 8;
        const QSizeF size(QFontMetrics(measurementFont()).horizontalAdvance(label) + 18, 26);
        const QRectF bounds = QRectF(rect()).adjusted(gap, gap, -gap, -gap);
        if (size.width() > bounds.width() || size.height() > bounds.height())
            return {};

        const qreal x = std::clamp(visible.right() - size.width(), bounds.left(), bounds.right() - size.width());
        const qreal below = visible.bottom() + gap;
        if (below + size.height() <= bounds.bottom())
            return QRectF(QPointF(x, below), size);

        const qreal above = visible.top() - gap - size.height();
        if (above >= bounds.top())
            return QRectF(QPointF(x, above), size);

        const qreal y = std::clamp(visible.bottom() - size.height() - gap, bounds.top(), bounds.bottom() - size.height());
        const qreal right = visible.right() + gap;
        if (right + size.width() <= bounds.right())
            return QRectF(QPointF(right, y), size);

        const qreal left = visible.left() - gap - size.width();
        return QRectF(QPointF(left >= bounds.left() ? left : x, y), size);
    }

    void drawMeasurement(QPainter &painter) const
    {
        painter.setFont(measurementFont());
        drawPanel(painter, badge_, 5);
        painter.setPen(panelText);
        painter.drawText(badge_, Qt::AlignCenter, measurement_);
    }

    Session *session_;
    const QImage image_;
    const QRectF layout_;
    const bool fit_;
    const QString instructionText_ = QStringLiteral("Drag to capture a region");
    const QFont instructionFont_ = theme::font(13, theme::Weight::Semibold);
    QRect instructions_;
    QPointF hover_{-1, -1};
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
#ifdef FREEZECUT_NATIVE_MONITOR_GEOMETRY
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
#ifdef FREEZECUT_NATIVE_MONITOR_GEOMETRY
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
    dragging_ = false;
    if (image.isNull())
        dismissOverlays();

    // Finish handling the mouse release before handing focus to the editor.
    QTimer::singleShot(0, this, [this, image = std::move(image)]() mutable {
        auto done = std::move(done_);
        done(std::move(image), [this] { dismissOverlays(); });
        dismissOverlays();
        deleteLater();
    });
}

void Session::dismissOverlays()
{
    for (auto *overlay : std::exchange(overlays_, {})) {
        overlay->hide();
        overlay->deleteLater();
    }
}

QPointF Session::cursorPoint(const Overlay *overlay, const QMouseEvent &event) const
{
    QPointF point = overlay->fitsImage()
        ? mapPoint(event.position(), overlay->viewport(), overlay->layout())
        : event.globalPosition();
#ifdef FREEZECUT_NATIVE_MONITOR_GEOMETRY
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

void RegionSelector::start(const QList<CaptureFrame> &frames, Completion done)
{
    if (done)
        (new Session(std::move(done)))->show(frames);
}
