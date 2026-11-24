#include "welcomewindow.h"

#include "controls.h"
#include "platform/reducedmotion.h"
#include "theme.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

constexpr QSizeF stageSize(600, 248);

qreal ease(qreal progress, qreal from, qreal to)
{
    static const QEasingCurve curve = [] {
        QEasingCurve result(QEasingCurve::BezierSpline);
        result.addCubicBezierSegment({0.23, 1}, {0.32, 1}, {1, 1});
        return result;
    }();
    return curve.valueForProgress(std::clamp((progress - from) / (to - from), 0., 1.));
}

QImage fieldMap()
{
    QImage image(800, 400, QImage::Format_RGB32);
    image.fill(QColor("#eee7d4"));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(2, 2);

    QPainterPath coast;
    coast.moveTo(0, 0);
    coast.lineTo(139, 0);
    coast.cubicTo(175, 37, 143, 67, 92, 74);
    coast.cubicTo(47, 85, 60, 99, 79, 120);
    coast.cubicTo(104, 155, 51, 182, 46, 200);
    coast.lineTo(0, 200);
    coast.closeSubpath();
    painter.fillPath(coast, QColor("#b6cbd5"));
    painter.setPen(QPen(QColor("#829eaf"), 1));
    painter.drawPath(coast);

    painter.setPen(QPen(QColor("#b2a48d"), 0.65));
    painter.setBrush(Qt::NoBrush);
    for (int ring = 1; ring <= 13; ++ring) {
        QPainterPath contour;
        const qreal radius = ring * 13.;
        for (int point = 0; point < 72; ++point) {
            const qreal angle = point * 2 * std::numbers::pi_v<qreal> / 72;
            const qreal ripple = 1 + .14 * std::sin(angle * 3 + .6) + .09 * std::cos(angle * 5 - ring * .12);
            const QPointF position(281 + (radius + 14) * ripple * std::cos(angle),
                                   63 + radius * .69 * ripple * std::sin(angle));
            if (point == 0)
                contour.moveTo(position);
            else
                contour.lineTo(position);
        }
        contour.closeSubpath();
        painter.drawPath(contour);
    }

    QPainterPath route;
    route.moveTo(100, 161);
    route.cubicTo(133, 137, 134, 120, 164, 122);
    route.cubicTo(212, 125, 201, 83, 235, 78);
    route.cubicTo(267, 71, 278, 104, 313, 94);
    route.cubicTo(331, 88, 345, 76, 359, 51);
    painter.setPen(QPen(QColor("#f9f4e8"), 5, Qt::SolidLine, Qt::RoundCap));
    painter.drawPath(route);
    painter.setPen(QPen(QColor("#475974"), 2, Qt::DashLine, Qt::RoundCap));
    painter.drawPath(route);
    painter.setPen(QPen(QColor("#f9f4e8"), 1.5));
    painter.setBrush(QColor("#475974"));
    painter.drawEllipse(QPointF(100, 161), 4, 4);
    painter.drawEllipse(QPointF(359, 51), 4, 4);
    return image;
}

void panel(QPainter &painter, const QRectF &rect, const QColor &color, qreal radius)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(rect, radius, radius);
}

void imageIn(QPainter &painter, const QImage &image, const QRectF &rect,
             const QRectF &source)
{
    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(rect, 5, 5);
    painter.setClipPath(clip, Qt::IntersectClip);
    painter.drawImage(rect, image, source);
    painter.restore();
}

void caption(QPainter &painter, const QString &text, const QRectF &rect, const QColor &color,
             int size = 12)
{
    painter.setFont(theme::font(size, theme::Weight::Semibold));
    painter.setPen(color);
    painter.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, text);
}

void paintArrow(QPainter &painter, const QLineF &arrow)
{
    const QColor color("#c62756");
    painter.setPen(QPen(color, 3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawLine(arrow);
    if (arrow.length() > 12) {
        QLineF tip = arrow.normalVector();
        tip.setLength(4.5);
        const QPointF back = arrow.p2() - (arrow.p2() - arrow.p1()) * (10 / arrow.length());
        const QPointF side = tip.p2() - tip.p1();
        painter.drawPolyline(QPolygonF{back + side, arrow.p2(), back - side});
    }
}

} // namespace

class WelcomePreview final : public QWidget {
public:
    explicit WelcomePreview(QWidget *parent)
        : QWidget(parent), map_(fieldMap())
    {
        setObjectName(QStringLiteral("welcomePreview"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumHeight(208);
        connect(&motion_, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            progress_ = value.toReal();
            update();
        });
        connect(&motion_, &QVariantAnimation::finished, this, [this] { previous_ = {}; });
    }

    void showStep(int step, bool entering = false)
    {
        motion_.stop();
        if (entering)
            reduceMotion_ = prefersReducedMotion();
        const bool animate = !reduceMotion_;
        previous_ = animate && !entering && isVisible() ? grab() : QPixmap{};
        direction_ = entering ? 0 : step < step_ ? -1 : 1;
        step_ = step;
        progress_ = animate ? 0 : 1;
        update();
        if (animate) {
            motion_.setDuration(entering ? 700 : 500);
            motion_.setStartValue(0.);
            motion_.setEndValue(1.);
            motion_.start();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
        const auto &colors = theme::colors();
        const QRectF bounds = QRectF(rect()).adjusted(.5, .5, -.5, -.5);
        painter.setPen(QPen(colors.border, 1));
        painter.setBrush(theme::mix(colors.stage, colors.accent, colors.dark ? .055 : .07));
        painter.drawRoundedRect(bounds, 13, 13);

        const qreal scale = std::min(width() / stageSize.width(), height() / stageSize.height());
        const qreal enter = ease(progress_, 0, .49);
        painter.save();
        painter.translate(bounds.center());
        painter.scale(scale, scale);
        painter.translate(-stageSize.width() / 2 + (1 - enter) * direction_ * 20,
                          -stageSize.height() / 2 + (direction_ == 0 ? (1 - enter) * 18 : 0));
        painter.setOpacity(.18 + enter * .82);
        if (step_ == 0)
            paintCapture(painter);
        else
            paintEditor(painter);
        painter.restore();

        if (!previous_.isNull()) {
            painter.setOpacity(1 - enter);
            painter.drawPixmap(0, 0, previous_);
        }
    }

private:
    void paintCapture(QPainter &painter) const
    {
        const auto &colors = theme::colors();
        const QRectF frame(96, 16, 408, 214);
        const QRectF content = frame.adjusted(8, 35, -8, -8);
        theme::paintShadow(painter, frame, 12, 11, 8, 1.25);
        panel(painter, frame, colors.surface, 12);
        paintIcon(painter, Icon::Screen, QRectF(111, 28, 14, 14), colors.textMuted);
        caption(painter, QStringLiteral("Trail map"), QRectF(133, 24, 140, 22), colors.text);
        imageIn(painter, map_, content, map_.rect());

        const QRectF target(166, 66, 268, 130);
        const qreal stretch = .23 + ease(progress_, .13, .88) * .77;
        const QRectF selection(target.topLeft(), target.size() * stretch);
        QPainterPath mask;
        mask.addRoundedRect(content, 5, 5);
        mask.addRect(selection);
        painter.fillPath(mask, QColor(18, 21, 33, 126));
        painter.setPen(QPen(theme::overlayAccent, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(selection);
        theme::paintBrackets(painter, selection.adjusted(-1, -1, 1, 1), 11, 2, Qt::white);

        const QPointF cursor = selection.bottomRight() + QPointF(4, 4);
        painter.setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(cursor - QPointF(6, 0), cursor + QPointF(6, 0));
        painter.drawLine(cursor - QPointF(0, 6), cursor + QPointF(0, 6));
    }

    void paintEditor(QPainter &painter) const
    {
        const auto &colors = theme::colors();
        const QRectF frame(62, 16, 430, 214);
        const QRectF content(104, 56, 376, 160);
        const QRectF crop(125, 32, 550, 303);
        theme::paintShadow(painter, frame, 12, 11, 8, 1.25);
        panel(painter, frame, colors.surface, 12);
        paintIcon(painter, Icon::Region, QRectF(75, 27, 15, 15), colors.accent);
        caption(painter, QStringLiteral("FreezeCut"), QRectF(98, 23, 104, 24), colors.text);
        paintIcon(painter, Icon::Copy, QRectF(406, 27, 14, 14), colors.textMuted);
        panel(painter, QRectF(433, 25, 44, 20), colors.accent, 5);
        caption(painter, QStringLiteral("Save"), QRectF(443, 25, 32, 20), colors.onAccent, 11);
        panel(painter, QRectF(71, 55, 26, 26), colors.accentSoft, 6);
        paintIcon(painter, Icon::Arrow, QRectF(76, 60, 16, 16), colors.accent);
        paintIcon(painter, Icon::Rectangle, QRectF(76, 95, 16, 16), colors.textMuted);
        paintIcon(painter, Icon::Blur, QRectF(76, 126, 16, 16), colors.textMuted);
        imageIn(painter, map_, content, crop);

        const QPointF origin(307, 175);
        const QPointF destination(344, 121);
        paintArrow(painter, QLineF(origin, origin + (destination - origin) * ease(progress_, .22, .87)));

        const qreal arrive = ease(progress_, .29, 1);
        painter.save();
        painter.setOpacity(painter.opacity() * arrive);
        painter.translate((1 - arrive) * 12, (1 - arrive) * 8);
        const QRectF library(428, 151, 120, 74);
        theme::paintShadow(painter, library, 10, 8, 5, 1.4);
        panel(painter, library, colors.raised, 10);
        paintIcon(painter, Icon::Library, QRectF(439, 160, 14, 14), colors.accent);
        caption(painter, QStringLiteral("Library"), QRectF(460, 157, 80, 21), colors.text, 11);
        imageIn(painter, map_, QRectF(439, 181, 48, 32), crop);
        caption(painter, QStringLiteral("Original"), QRectF(493, 181, 50, 32), colors.textMuted, 10);
        painter.restore();
    }

    const QImage map_;
    QVariantAnimation motion_;
    QPixmap previous_;
    qreal progress_ = 1;
    int step_ = 0;
    int direction_ = 0;
    bool reduceMotion_ = false;
};

WelcomeWindow::WelcomeWindow(QWidget *parent)
    : QDialog(parent),
      preview_(new WelcomePreview(this)),
      heading_(theme::label({}, "welcomeHeading", this)),
      description_(theme::label({}, "welcomeDescription", this)),
      back_(new Button(Button::Ghost, Icon::Back, QStringLiteral("Back"), this)),
      next_(new Button(Button::Primary, Icon::None, QStringLiteral("Continue"), this))
{
    setObjectName(QStringLiteral("welcome"));
    setWindowTitle(QStringLiteral("Welcome to FreezeCut"));
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setMinimumSize(500, 414);
    resize(660, 456);
    theme::attach(this, R"(
        QDialog#welcome { background: @surface; }
        QLabel#welcomeHeading { font-size: 33px; font-weight: 600; }
        QLabel#welcomeDescription { color: @textMuted; font-size: 15px; }
    )");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 18);
    layout->setSpacing(18);

    auto *intro = new QHBoxLayout;
    intro->setSpacing(14);
    auto *logo = new QLabel(this);
    logo->setPixmap(QApplication::windowIcon().pixmap(QSize(42, 42), devicePixelRatioF()));
    intro->addWidget(logo, 0, Qt::AlignTop);
    auto *copy = new QVBoxLayout;
    copy->setSpacing(5);
    heading_->setWordWrap(true);
    description_->setWordWrap(true);
    copy->addWidget(heading_);
    copy->addWidget(description_);
    intro->addLayout(copy, 1);
    layout->addLayout(intro);
    layout->addWidget(preview_, 1);

    auto *footer = new QHBoxLayout;
    footer->setSpacing(8);
    auto *open = new Button(Button::Ghost, Icon::None, QStringLiteral("Open FreezeCut"), this);
    for (auto *button : {back_, open, next_})
        button->setLarge(true);
    next_->setDefault(true);
    back_->setAutoDefault(false);
    open->setAutoDefault(false);
    setTabOrder(back_, open);
    setTabOrder(open, next_);
    footer->addWidget(back_);
    footer->addStretch();
    footer->addWidget(open);
    footer->addWidget(next_);
    layout->addLayout(footer);

    connect(back_, &QPushButton::clicked, this, [this] { showStep(0); });
    connect(open, &QPushButton::clicked, this, [this] { done(OpenApp); });
    connect(next_, &QPushButton::clicked, this, [this] {
        if (step_ == 0)
            showStep(1);
        else
            done(TryCapture);
    });
    showStep(0);
}

void WelcomeWindow::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    preview_->showStep(step_, true);
}

void WelcomeWindow::showStep(int step)
{
    step_ = step;
    const bool capture = step == 0;
    heading_->setText(capture ? QStringLiteral("Capture just what you need")
                             : QStringLiteral("Make it clear"));
    description_->setText(capture ? QStringLiteral("Choose a region or grab the whole screen.")
                                 : QStringLiteral("Mark it up, copy the result, or reopen the original in Library."));
    back_->setVisible(!capture);
    next_->setText(capture ? QStringLiteral("Continue") : QStringLiteral("Try a capture"));
    if (isVisible()) {
        preview_->showStep(step);
        next_->setFocus(Qt::OtherFocusReason);
    }
}
