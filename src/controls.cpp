#include "controls.h"

#include "theme.h"

#include <QEnterEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include <algorithm>
#include <initializer_list>

namespace {

QVariantAnimation *animation(QObject *owner, int milliseconds)
{
    auto *result = new QVariantAnimation(owner);
    result->setDuration(milliseconds);
    result->setEasingCurve(QEasingCurve::OutCubic);
    return result;
}

void animateTo(QVariantAnimation *animation, const QVariant &from, const QVariant &to)
{
    animation->stop();
    animation->setStartValue(from);
    animation->setEndValue(to);
    animation->start();
}

class Separator final : public QWidget {
public:
    using QWidget::QWidget;
    QSize sizeHint() const override { return QSize(11, 24); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(QRectF(width() / 2.0 - 0.5, 4, 1, height() - 8), theme::colors().border);
    }
};

} // namespace

QWidget *separator(QWidget *parent)
{
    return new Separator(parent);
}

void paintIcon(QPainter &painter, Icon icon, const QRectF &rect, const QColor &color, qreal stroke)
{
    if (icon == Icon::None) {
        return;
    }
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal scale = rect.width() / 24.0;
    painter.translate(rect.topLeft());
    painter.scale(scale, scale);
    painter.setPen(QPen(color, stroke / scale, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    const auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2) { painter.drawLine(QPointF(x1, y1), QPointF(x2, y2)); };
    const auto polyline = [&](std::initializer_list<QPointF> points) { painter.drawPolyline(QPolygonF(QList<QPointF>(points))); };
    const auto solid = [&] {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
    };
    switch (icon) {
    case Icon::None:
        break;
    case Icon::Region:
        polyline({{4, 9}, {4, 4}, {9, 4}});
        polyline({{15, 4}, {20, 4}, {20, 9}});
        polyline({{4, 15}, {4, 20}, {9, 20}});
        polyline({{15, 20}, {20, 20}, {20, 15}});
        solid();
        painter.drawEllipse(QPointF(12, 12), 1.9, 1.9);
        break;
    case Icon::Screen:
        painter.drawRoundedRect(QRectF(3, 4.5, 18, 12.5), 2, 2);
        line(9, 20, 15, 20);
        line(12, 17, 12, 20);
        break;
    case Icon::Library:
        painter.drawRoundedRect(QRectF(3.5, 8, 17, 12), 2, 2);
        line(6.5, 5, 17.5, 5);
        polyline({{6.5, 17}, {10, 13.5}, {12.5, 16}, {14.5, 14}, {17.5, 17}});
        break;
    case Icon::Settings:
        line(4, 8, 6.6, 8);
        line(11.4, 8, 20, 8);
        painter.drawEllipse(QPointF(9, 8), 2.4, 2.4);
        line(4, 16, 12.6, 16);
        line(17.4, 16, 20, 16);
        painter.drawEllipse(QPointF(15, 16), 2.4, 2.4);
        break;
    case Icon::Arrow:
        line(5.5, 18.5, 17.5, 6.5);
        polyline({{10, 6}, {18, 6}, {18, 14}});
        break;
    case Icon::Pen:
        painter.drawPolygon(QPolygonF{QPointF(4, 20), QPointF(5, 15), QPointF(15.5, 4.5), QPointF(19.5, 8.5), QPointF(9, 19)});
        line(13.5, 6.5, 17.5, 10.5);
        break;
    case Icon::Rectangle:
        painter.drawRoundedRect(QRectF(4, 5.5, 16, 13), 2, 2);
        break;
    case Icon::Blur:
        solid();
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                const qreal radius = 1.2 + 0.35 * ((row + column) % 3);
                painter.drawEllipse(QPointF(7 + column * 5, 7 + row * 5), radius, radius);
            }
        }
        break;
    case Icon::Redact:
        painter.drawRoundedRect(QRectF(3.5, 6, 17, 12), 2.5, 2.5);
        solid();
        painter.drawRoundedRect(QRectF(7, 10, 10, 4), 1, 1);
        break;
    case Icon::Redo:
        painter.translate(24, 0);
        painter.scale(-1, 1);
        [[fallthrough]];
    case Icon::Undo: {
        polyline({{8.5, 4.5}, {4.5, 8.5}, {8.5, 12.5}});
        QPainterPath path(QPointF(5, 8.5));
        path.lineTo(14, 8.5);
        path.cubicTo(17.5, 8.5, 20, 11, 20, 14.25);
        path.cubicTo(20, 17.5, 17.5, 20, 14, 20);
        path.lineTo(10, 20);
        painter.drawPath(path);
        break;
    }
    case Icon::Copy:
        painter.drawRoundedRect(QRectF(8.5, 8.5, 11.5, 11.5), 2, 2);
        polyline({{5.5, 15.5}, {5, 15.5}, {4, 14.5}, {4, 5}, {5, 4}, {14.5, 4}, {15.5, 5}, {15.5, 5.5}});
        break;
    case Icon::Save:
        line(12, 4, 12, 14.5);
        polyline({{7.5, 10}, {12, 14.5}, {16.5, 10}});
        polyline({{4, 15}, {4, 19}, {5, 20}, {19, 20}, {20, 19}, {20, 15}});
        break;
    case Icon::Plus:
        line(12, 6, 12, 18);
        [[fallthrough]];
    case Icon::Minus:
        line(6, 12, 18, 12);
        break;
    case Icon::Back:
        polyline({{14.5, 6}, {8.5, 12}, {14.5, 18}});
        break;
    case Icon::Forward:
        polyline({{9.5, 6}, {15.5, 12}, {9.5, 18}});
        break;
    case Icon::Refresh:
        painter.drawArc(QRectF(4.5, 4.5, 15, 15), 30 * 16, 290 * 16);
        polyline({{19.8, 3.8}, {18.6, 8.6}, {13.9, 7.4}});
        break;
    case Icon::Check:
        polyline({{5, 12.5}, {10, 17.5}, {19, 7}});
        break;
    case Icon::Warning:
        painter.drawPolygon(QPolygonF{QPointF(12, 4), QPointF(21, 19.5), QPointF(3, 19.5)});
        line(12, 10, 12, 14);
        solid();
        painter.drawEllipse(QPointF(12, 16.9), 1.1, 1.1);
        break;
    }
    painter.restore();
}

// MARK: Button

Button::Button(Kind kind, Icon icon, const QString &text, QWidget *parent)
    : QPushButton(text, parent), kind_(kind), icon_(icon), hoverAnimation_(animation(this, 140))
{
    setAttribute(Qt::WA_Hover);
    // Clicking a button must not take keyboard focus from the canvas or the list.
    setFocusPolicy(Qt::TabFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    if (!text.isEmpty()) {
        setAccessibleName(text);
    }
    connect(hoverAnimation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        hover_ = value.toReal();
        update();
    });
}

void Button::setHint(const QString &hint)
{
    hint_ = hint;
    updateGeometry();
    update();
}

void Button::setLarge(bool large)
{
    large_ = large;
    updateGeometry();
    update();
}

void Button::setCompact(bool compact)
{
    if (compact_ == compact) {
        return;
    }
    compact_ = compact;
    updateGeometry();
    update();
}

void Button::setCheckedDrawnByParent(bool value)
{
    checkedDrawnByParent_ = value;
    update();
}

bool Button::showsLabel() const
{
    return !text().isEmpty() && (!compact_ || icon_ == Icon::None);
}

QSize Button::sizeHint() const
{
    const int height = large_ ? 40 : theme::controlHeight;
    if (!showsLabel()) {
        return QSize(height, height);
    }
    const QFont font = theme::font(large_ ? 14 : 13, large_ ? theme::Weight::Semibold : theme::Weight::Medium);
    int width = QFontMetrics(font).horizontalAdvance(text()) + (large_ ? 36 : 24);
    if (icon_ != Icon::None) {
        width += (large_ ? 18 : 16) + 7;
    }
    if (!hint_.isEmpty()) {
        width += 12 + theme::keycapSize(hint_).width();
    }
    return QSize(std::max(width, height), height);
}

QSize Button::minimumSizeHint() const
{
    return sizeHint();
}

void Button::paintEvent(QPaintEvent *)
{
    const theme::Palette &c = theme::colors();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled()) {
        painter.setOpacity(0.38);
    }
    const QRectF frame = QRectF(rect());
    const bool checkedHere = isChecked() && !checkedDrawnByParent_;
    QColor fill;
    QColor foreground;
    switch (kind_) {
    case Primary:
        fill = theme::mix(c.accent, c.accentHover, hover_);
        if (isDown()) {
            fill = theme::mix(c.accent, Qt::black, 0.12);
        }
        foreground = c.onAccent;
        break;
    case Tonal:
        fill = theme::mix(c.control, c.controlHover, hover_);
        if (isDown()) {
            fill = c.controlPressed;
        }
        foreground = c.text;
        break;
    case Ghost:
        fill = theme::withAlpha(c.controlHover, hover_ * (checkedHere ? 0.5 : 1));
        if (checkedHere) {
            fill = theme::mix(c.accentSoft, theme::withAlpha(c.accent, 0.26), hover_);
        }
        if (isDown()) {
            fill = c.controlPressed;
        }
        foreground = isChecked() ? c.accent : showsLabel() ? c.text : theme::mix(c.textMuted, c.text, hover_);
        break;
    }
    if (fill.alpha() > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(frame, theme::radius, theme::radius);
    }

    const int iconSize = large_ ? 18 : 16;
    const QFont font = theme::font(large_ ? 14 : 13, large_ ? theme::Weight::Semibold : theme::Weight::Medium);
    const QFontMetrics metrics(font);
    const bool label = showsLabel();
    const QSize cap = theme::keycapSize(hint_);
    int content = 0;
    if (icon_ != Icon::None) {
        content += iconSize;
    }
    if (label) {
        content += metrics.horizontalAdvance(text()) + (icon_ != Icon::None ? 7 : 0);
    }
    if (label && !hint_.isEmpty()) {
        content += 12 + cap.width();
    }
    qreal x = (width() - content) / 2.0;
    if (icon_ != Icon::None) {
        paintIcon(painter, icon_, QRectF(x, (height() - iconSize) / 2.0, iconSize, iconSize), foreground,
                  large_ ? 1.8 : 1.6);
        x += iconSize + 7;
    }
    if (label) {
        painter.setFont(font);
        painter.setPen(foreground);
        const int textWidth = metrics.horizontalAdvance(text());
        painter.drawText(QRectF(x, 0, textWidth + 1, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
        x += textWidth + 12;
        if (!hint_.isEmpty()) {
            const QRectF capRect(x, (height() - cap.height()) / 2.0, cap.width(), cap.height());
            if (kind_ == Primary) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(theme::withAlpha(c.onAccent, 0.14));
                painter.drawRoundedRect(capRect, 6, 6);
                painter.setPen(theme::withAlpha(c.onAccent, 0.8));
                painter.setFont(theme::font(11, theme::Weight::Medium));
                painter.drawText(capRect, Qt::AlignCenter, hint_);
            } else {
                theme::paintKeycap(painter, capRect, hint_);
            }
        }
    }
    if (hasFocus() && keyboardFocus_) {
        theme::paintFocusRing(painter, frame, theme::radius);
    }
}

void Button::enterEvent(QEnterEvent *event)
{
    QPushButton::enterEvent(event);
    if (isEnabled()) {
        animateHover(1);
    }
}

void Button::leaveEvent(QEvent *event)
{
    QPushButton::leaveEvent(event);
    animateHover(0);
}

void Button::focusInEvent(QFocusEvent *event)
{
    keyboardFocus_ = event->reason() == Qt::TabFocusReason || event->reason() == Qt::BacktabFocusReason;
    QPushButton::focusInEvent(event);
}

void Button::changeEvent(QEvent *event)
{
    QPushButton::changeEvent(event);
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        hoverAnimation_->stop();
        hover_ = 0;
        update();
    }
}

void Button::animateHover(qreal target)
{
    if (!isVisible()) {
        hover_ = target;
        return;
    }
    animateTo(hoverAnimation_, hover_, target);
}

// MARK: Toggle

Toggle::Toggle(const QString &text, QWidget *parent) : QAbstractButton(parent), animation_(animation(this, 180))
{
    setText(text);
    setAccessibleName(text);
    setCheckable(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        position_ = value.toReal();
        update();
    });
    connect(this, &QAbstractButton::toggled, this, [this] { animate(); });
}

QSize Toggle::sizeHint() const
{
    return QSize(QFontMetrics(theme::font(13)).horizontalAdvance(text()) + 16 + 36, 30);
}

void Toggle::animate()
{
    const qreal target = isChecked() ? 1 : 0;
    if (!isVisible()) {
        animation_->stop();
        position_ = target;
        update();
        return;
    }
    animateTo(animation_, position_, target);
}

void Toggle::paintEvent(QPaintEvent *)
{
    const theme::Palette &c = theme::colors();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled()) {
        painter.setOpacity(0.4);
    }
    const QRectF track(width() - 36, (height() - 20) / 2.0, 36, 20);
    painter.setFont(theme::font(13));
    painter.setPen(c.text);
    const int textWidth = int(track.left()) - 16;
    painter.drawText(QRect(0, 0, textWidth, height()), Qt::AlignVCenter | Qt::AlignLeft,
                     QFontMetrics(painter.font()).elidedText(text(), Qt::ElideRight, textWidth));

    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::mix(c.borderStrong, c.accent, position_));
    painter.drawRoundedRect(track, 10, 10);
    const QPointF knob(track.left() + 10 + position_ * 16, track.center().y());
    painter.setBrush(QColor(0, 0, 0, 40));
    painter.drawEllipse(knob + QPointF(0, 0.75), 8, 8);
    painter.setBrush(c.dark ? theme::mix(QColor(Qt::white), c.onAccent, position_ * 0.06) : QColor(Qt::white));
    painter.drawEllipse(knob, 8, 8);
    if (hasFocus()) {
        theme::paintFocusRing(painter, track.adjusted(-3, -3, 3, 3), 13);
    }
}

// MARK: Segmented

Segmented::Segmented(const QStringList &labels, QWidget *parent)
    : QWidget(parent), labels_(labels), animation_(animation(this, 200))
{
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        thumb_ = value.toRectF();
        update();
    });
}

QRectF Segmented::segment(int index) const
{
    const qreal width = (this->width() - 4.0) / std::max<qsizetype>(1, labels_.size());
    return QRectF(2 + index * width, 2, width, height() - 4);
}

void Segmented::setCurrent(int index)
{
    if (index < 0 || index >= labels_.size() || index == current_) {
        return;
    }
    current_ = index;
    if (isVisible()) {
        animateTo(animation_, thumb_, segment(index));
    } else {
        thumb_ = segment(index);
    }
    update();
    emit currentChanged(index);
}

QSize Segmented::sizeHint() const
{
    const QFontMetrics metrics(theme::font(13, theme::Weight::Medium));
    int widest = 0;
    for (const QString &label : labels_) {
        widest = std::max(widest, metrics.horizontalAdvance(label));
    }
    return QSize(int((widest + 28) * labels_.size()) + 4, theme::controlHeight);
}

void Segmented::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    animation_->stop();
    thumb_ = segment(current_);
}

void Segmented::mousePressEvent(QMouseEvent *event)
{
    for (int index = 0; index < labels_.size(); ++index) {
        if (segment(index).contains(event->position())) {
            setCurrent(index);
            return;
        }
    }
}

void Segmented::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Up) {
        setCurrent(current_ - 1);
    } else if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Down) {
        setCurrent(current_ + 1);
    } else {
        QWidget::keyPressEvent(event);
    }
}

void Segmented::paintEvent(QPaintEvent *)
{
    const theme::Palette &c = theme::colors();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (!isEnabled()) {
        painter.setOpacity(0.4);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(c.control);
    painter.drawRoundedRect(QRectF(rect()), theme::radius, theme::radius);
    if (!c.dark) {
        theme::paintShadow(painter, thumb_, 6, 3, 1, 0.8);
    }
    painter.setBrush(c.dark ? theme::mix(c.controlHover, QColor(Qt::white), 0.06) : c.raised);
    painter.drawRoundedRect(thumb_, 6, 6);
    for (int index = 0; index < labels_.size(); ++index) {
        const bool selected = index == current_;
        painter.setFont(theme::font(13, selected ? theme::Weight::Semibold : theme::Weight::Medium));
        painter.setPen(selected ? c.text : c.textMuted);
        painter.drawText(segment(index), Qt::AlignCenter, labels_.at(index));
    }
    if (hasFocus()) {
        theme::paintFocusRing(painter, QRectF(rect()), theme::radius);
    }
}

// MARK: Island

Island::Island(QWidget *parent) : QWidget(parent), row_(new QHBoxLayout(this))
{
    constexpr int inset = shadowMargin + 6;
    row_->setContentsMargins(inset, inset, inset, inset);
    row_->setSpacing(2);
    row_->setSizeConstraint(QLayout::SetFixedSize);
}

void Island::addSeparator()
{
    row_->addWidget(new Separator(this));
}

QRectF Island::panel() const
{
    return QRectF(rect()).adjusted(shadowMargin, shadowMargin, -shadowMargin, -shadowMargin);
}

void Island::paintEvent(QPaintEvent *)
{
    const theme::Palette &c = theme::colors();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF body = panel();
    theme::paintShadow(painter, body, 14, 10, 3, c.dark ? 1.4 : 1);
    painter.setPen(QPen(c.dark ? c.border : theme::withAlpha(c.borderStrong, 0.6), 1));
    painter.setBrush(c.raised);
    painter.drawRoundedRect(body.adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
    paintPanel(painter);
}

void Island::mousePressEvent(QMouseEvent *event)
{
    event->accept();
}

// MARK: Notice

Notice::Notice(Tone tone, QWidget *parent) : QWidget(parent), tone_(tone), fade_(animation(this, 160)), hold_(new QTimer(this))
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    hide();
    connect(fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        opacity_ = value.toReal();
        update();
    });
    connect(fade_, &QVariantAnimation::finished, this, [this] {
        if (opacity_ < 0.02) {
            hide();
        }
    });
    hold_->setSingleShot(true);
    hold_->setInterval(1600);
    connect(hold_, &QTimer::timeout, this, [this] {
        fade_->setDuration(320);
        animateTo(fade_, opacity_, 0.0);
    });
}

void Notice::setMessage(const QString &message)
{
    hold_->stop();
    fade_->stop();
    opacity_ = 1;
    message_ = message;
    setVisible(!message.isEmpty());
    if (!message.isEmpty()) {
        raise();
    }
    update();
    emit messageChanged();
}

void Notice::flash(const QString &message)
{
    const qreal from = isVisible() && opacity_ > 0.5 ? opacity_ : 0.0;
    message_ = message;
    opacity_ = from;
    show();
    raise();
    emit messageChanged();
    fade_->setDuration(140);
    animateTo(fade_, from, 1.0);
    hold_->start();
}

QSize Notice::sizeFor(int maxWidth) const
{
    const QFontMetrics metrics(theme::font(13, theme::Weight::Medium));
    const int chrome = 2 * shadowMargin + 16 + 16 + 8 + 16;
    const int textMax = std::max(80, maxWidth - chrome);
    const QRect text = metrics.boundingRect(QRect(0, 0, textMax, 2000), Qt::TextWordWrap, message_);
    return QSize(std::min(maxWidth, text.width() + chrome + 2), std::max(36, text.height() + 18) + 2 * shadowMargin);
}

void Notice::paintEvent(QPaintEvent *)
{
    const theme::Palette &c = theme::colors();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setOpacity(opacity_);
    const QRectF body = QRectF(rect()).adjusted(shadowMargin, shadowMargin, -shadowMargin, -shadowMargin);
    const qreal radius = body.height() <= 40 ? body.height() / 2 : 14;
    theme::paintShadow(painter, body, radius, 8, 2, c.dark ? 1.3 : 0.9);
    const bool warning = tone_ == Warning;
    painter.setPen(QPen(warning ? theme::withAlpha(c.warning, 0.4) : c.border, 1));
    painter.setBrush(warning ? theme::mix(c.raised, c.warning, c.dark ? 0.13 : 0.09) : c.raised);
    painter.drawRoundedRect(body.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    const QRectF icon(body.left() + 16, body.top() + 10, 16, 16);
    paintIcon(painter, warning ? Icon::Warning : Icon::Check, icon, warning ? c.warning : c.accent, 1.8);
    painter.setPen(c.text);
    painter.setFont(theme::font(13, theme::Weight::Medium));
    const QRectF text = body.adjusted(16 + 16 + 8, 9, -16, -9);
    painter.drawText(text, Qt::TextWordWrap | Qt::AlignVCenter | Qt::AlignLeft, message_);
}

// MARK: Banner

Banner::Banner(QWidget *parent) : QWidget(parent), text_(theme::label({}, "body", this))
{
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(16 + 16 + 10, 9, 16, 9);
    text_->setWordWrap(true);
    text_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    row->addWidget(text_, 1);
    hide();
}

void Banner::setMessage(const QString &message)
{
    text_->setText(message);
    setVisible(!message.isEmpty());
}

void Banner::paintEvent(QPaintEvent *)
{
    const theme::Palette &c = theme::colors();
    QPainter painter(this);
    painter.fillRect(rect(), theme::mix(c.surface, c.warning, c.dark ? 0.11 : 0.10));
    painter.fillRect(QRectF(0, height() - 1, width(), 1), theme::withAlpha(c.warning, 0.3));
    const int line = text_->fontMetrics().height();
    paintIcon(painter, Icon::Warning, QRectF(16, 9 + (line - 16) / 2.0, 16, 16), c.warning, 1.8);
}

// MARK: EmptyState

class EmptyState::Glyph final : public QWidget {
public:
    using QWidget::QWidget;
    Icon icon = Icon::None;
    QSize sizeHint() const override { return QSize(76, 76); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const theme::Palette &c = theme::colors();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        theme::paintBrackets(painter, QRectF(rect()), 16, 2, theme::withAlpha(c.accent, 0.85));
        paintIcon(painter, icon, QRectF(width() / 2.0 - 15, height() / 2.0 - 15, 30, 30), c.textMuted, 1.7);
    }
};

EmptyState::EmptyState(QWidget *parent)
    : QWidget(parent), glyph_(new Glyph(this)), title_(theme::label({}, "emptyTitle", this)),
      details_(theme::label({}, "muted", this)), action_(new Button(Button::Primary, Icon::None, {}, this))
{
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(24, 24, 24, 24);
    column->setSpacing(0);
    column->addStretch(4);
    column->addWidget(glyph_, 0, Qt::AlignHCenter);
    column->addSpacing(22);
    // Wrapped labels only get their full height when the layout does not align them, so centre
    // each one between stretches instead.
    for (QLabel *label : {title_, details_}) {
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        label->setFixedWidth(400);
        auto *row = new QHBoxLayout;
        row->addStretch();
        row->addWidget(label);
        row->addStretch();
        column->addLayout(row);
        if (label == title_) {
            column->addSpacing(8);
        }
    }
    column->addSpacing(20);
    action_->setLarge(true);
    column->addWidget(action_, 0, Qt::AlignHCenter);
    column->addStretch(5);
}

void EmptyState::set(Icon icon, const QString &title, const QString &details, const QString &action)
{
    glyph_->icon = icon;
    glyph_->update();
    title_->setText(title);
    details_->setText(details);
    details_->setVisible(!details.isEmpty());
    action_->setText(action);
    action_->setAccessibleName(action);
    action_->setVisible(!action.isEmpty());
}
