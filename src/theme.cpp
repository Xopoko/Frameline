#include "theme.h"

#include <QApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QStyleHints>
#include <QWidget>

#include <array>
#include <utility>

namespace theme {

namespace {

const Palette darkPalette{
    .dark = true,
    .stage = QColor(0x0e, 0x12, 0x16),
    .surface = QColor(0x15, 0x1a, 0x20),
    .raised = QColor(0x1c, 0x22, 0x2a),
    .inset = QColor(0x0e, 0x12, 0x16),
    .control = QColor(0x26, 0x2e, 0x38),
    .controlHover = QColor(0x30, 0x3a, 0x46),
    .controlPressed = QColor(0x22, 0x29, 0x32),
    .border = QColor(0x2c, 0x35, 0x40),
    .borderStrong = QColor(0x42, 0x4e, 0x5c),
    .text = QColor(0xee, 0xf2, 0xf5),
    .textMuted = QColor(0x9e, 0xaa, 0xb7),
    .textFaint = QColor(0x68, 0x74, 0x81),
    .accent = QColor(0x5f, 0xe3, 0xb5),
    .accentHover = QColor(0x80, 0xec, 0xc7),
    .onAccent = QColor(0x05, 0x24, 0x1a),
    .accentSoft = QColor(0x5f, 0xe3, 0xb5, 38),
    .warning = QColor(0xf5, 0xbd, 0x5c),
    .danger = QColor(0xff, 0x7a, 0x7a),
    .shadow = QColor(0, 0, 0, 120),
};

const Palette lightPalette{
    .dark = false,
    .stage = QColor(0xe7, 0xea, 0xee),
    .surface = QColor(0xf5, 0xf6, 0xf8),
    .raised = QColor(0xff, 0xff, 0xff),
    .inset = QColor(0xff, 0xff, 0xff),
    .control = QColor(0xe9, 0xec, 0xf0),
    .controlHover = QColor(0xdd, 0xe2, 0xe7),
    .controlPressed = QColor(0xd3, 0xd9, 0xdf),
    .border = QColor(0xd7, 0xdc, 0xe2),
    .borderStrong = QColor(0xb9, 0xc1, 0xca),
    .text = QColor(0x13, 0x18, 0x1d),
    .textMuted = QColor(0x52, 0x5e, 0x6a),
    .textFaint = QColor(0x8d, 0x97, 0xa2),
    .accent = QColor(0x0a, 0x7a, 0x58),
    .accentHover = QColor(0x0c, 0x8b, 0x64),
    .onAccent = QColor(0xff, 0xff, 0xff),
    .accentSoft = QColor(0x0a, 0x7a, 0x58, 30),
    .warning = QColor(0x94, 0x5d, 0x00),
    .danger = QColor(0xc2, 0x2f, 0x3a),
    .shadow = QColor(0x1c, 0x28, 0x36, 46),
};

bool systemPrefersDark()
{
    const QByteArray forced = qgetenv("FRAMELINE_THEME").toLower();
    if (forced == "light" || forced == "dark") {
        return forced == "dark";
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (qGuiApp) {
        return QGuiApplication::styleHints()->colorScheme() != Qt::ColorScheme::Light;
    }
#endif
    return true;
}

bool &darkMode()
{
    static bool dark = systemPrefersDark();
    return dark;
}

QString css(const QColor &color)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alpha());
}

constexpr auto sharedRules = R"(
    QWidget#stage { background: @stage; }
    QWidget#surface { background: @surface; }
    QLabel { color: @text; font-size: 13px; }
    QLabel:disabled { color: @textFaint; }
    QLabel#title { font-size: 22px; font-weight: 600; }
    QLabel#heading { font-size: 15px; font-weight: 600; }
    QLabel#muted { color: @textMuted; font-size: 13px; }
    QLabel#caption { color: @textMuted; font-size: 12px; }
    QLabel#emptyTitle { font-size: 18px; font-weight: 600; }
    QLineEdit {
        background: @inset; border: 1px solid @border; border-radius: 8px; padding: 6px 10px;
        color: @text; font-size: 13px; min-height: 18px;
        selection-background-color: @accent; selection-color: @onAccent;
    }
    QLineEdit:hover { border-color: @borderStrong; }
    QLineEdit:focus { border-color: @accent; }
    QLineEdit:disabled { color: @textFaint; background: @surface; }
    QScrollArea, QAbstractScrollArea { background: transparent; border: none; }
    QToolTip {
        background: @raised; color: @text; border: 1px solid @borderStrong; border-radius: 6px;
        padding: 4px 8px; font-size: 12px;
    }
    QScrollBar:vertical { background: transparent; width: 12px; margin: 2px; }
    QScrollBar:horizontal { background: transparent; height: 12px; margin: 2px; }
    QScrollBar::handle { background: @borderStrong; border-radius: 4px; min-width: 32px; min-height: 32px; }
    QScrollBar::handle:hover { background: @textFaint; }
    QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
    QScrollBar::add-page, QScrollBar::sub-page { background: none; }
)";

QString resolved(const QString &rules)
{
    const Palette &c = colors();
    // Longer names first, so @border does not match inside @borderStrong.
    const std::array<std::pair<const char *, QColor>, 18> entries{{
        {"@controlPressed", c.controlPressed}, {"@controlHover", c.controlHover}, {"@borderStrong", c.borderStrong},
        {"@accentHover", c.accentHover}, {"@accentSoft", c.accentSoft},
        {"@textMuted", c.textMuted}, {"@textFaint", c.textFaint}, {"@onAccent", c.onAccent},
        {"@surface", c.surface}, {"@control", c.control}, {"@warning", c.warning}, {"@accent", c.accent},
        {"@border", c.border}, {"@raised", c.raised}, {"@danger", c.danger}, {"@stage", c.stage},
        {"@inset", c.inset}, {"@text", c.text},
    }};
    QString result = rules;
    for (const auto &[name, color] : entries) {
        result.replace(QLatin1String(name), css(color));
    }
    return result;
}

} // namespace

const Palette &colors()
{
    return darkMode() ? darkPalette : lightPalette;
}

Notifier *notifier()
{
    static Notifier *instance = [] {
        auto *created = new Notifier;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (qGuiApp) {
            created->setParent(qGuiApp);
            QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, created, [created] {
                const bool dark = systemPrefersDark();
                if (dark != darkMode()) {
                    darkMode() = dark;
                    emit created->changed();
                }
            });
        }
#endif
        return created;
    }();
    return instance;
}

void attach(QWidget *root, const char *windowRules)
{
    const QString rules = QString::fromLatin1(sharedRules) + QString::fromLatin1(windowRules);
    root->setStyleSheet(resolved(rules));
    QObject::connect(notifier(), &Notifier::changed, root, [root = QPointer<QWidget>(root), rules] {
        if (!root) {
            return;
        }
        root->setStyleSheet(resolved(rules));
        root->update();
        for (auto *child : root->findChildren<QWidget *>()) {
            child->update();
        }
    });
}

QFont font(int pixelSize, Weight weight, bool tabular)
{
    QFont result = QApplication::font();
    result.setPixelSize(pixelSize);
    result.setWeight(weight == Weight::Semibold ? QFont::DemiBold : weight == Weight::Medium ? QFont::Medium : QFont::Normal);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    if (tabular) {
        result.setFeature(QFont::Tag("tnum"), 1);
    }
#else
    Q_UNUSED(tabular);
#endif
    return result;
}

QLabel *label(const QString &text, const char *role, QWidget *parent)
{
    auto *result = new QLabel(text, parent);
    result->setObjectName(QString::fromLatin1(role));
    result->setTextFormat(Qt::PlainText);
    return result;
}

void paintShadow(QPainter &painter, const QRectF &rect, qreal cornerRadius, int spread, qreal offset, qreal strength)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    const QColor base = colors().shadow;
    // Concentric rings with a quadratic falloff stand in for a blur, which QPainter lacks.
    for (int ring = 0; ring < spread; ++ring) {
        const qreal fade = 1 - qreal(ring) / spread;
        QColor color = base;
        color.setAlphaF(std::min(1.0, base.alphaF() * strength * fade * fade * 0.45));
        painter.setPen(QPen(color, 1.5));
        const QRectF outline = rect.adjusted(-ring, -ring, ring, ring).translated(0, offset * (0.4 + 0.6 * ring / spread));
        painter.drawRoundedRect(outline, cornerRadius + ring, cornerRadius + ring);
    }
    painter.restore();
}

void paintBrackets(QPainter &painter, const QRectF &rect, qreal length, qreal width, const QColor &color)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    const QRectF r = rect.adjusted(width / 2, width / 2, -width / 2, -width / 2);
    const std::array<std::pair<QPointF, QPointF>, 4> corners{{
        {r.topLeft(), {1, 1}}, {r.topRight(), {-1, 1}}, {r.bottomLeft(), {1, -1}}, {r.bottomRight(), {-1, -1}},
    }};
    for (const auto &[corner, direction] : corners) {
        painter.drawPolyline(QPolygonF{corner + QPointF(direction.x() * length, 0), corner,
                                       corner + QPointF(0, direction.y() * length)});
    }
    painter.restore();
}

void paintFocusRing(QPainter &painter, const QRectF &rect, qreal cornerRadius)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(colors().accent, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), cornerRadius, cornerRadius);
    painter.restore();
}

QSize keycapSize(const QString &keys)
{
    if (keys.isEmpty()) {
        return {};
    }
    return QSize(QFontMetrics(font(11, Weight::Medium)).horizontalAdvance(keys) + 14, 22);
}

void paintKeycap(QPainter &painter, const QRectF &rect, const QString &keys)
{
    const Palette &c = colors();
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(c.border, 1));
    painter.setBrush(c.control);
    painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    painter.setPen(c.textMuted);
    painter.setFont(font(11, Weight::Medium));
    painter.drawText(rect, Qt::AlignCenter, keys);
    painter.restore();
}

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(alpha);
    return color;
}

QColor mix(const QColor &from, const QColor &to, qreal amount)
{
    const qreal keep = 1 - amount;
    return QColor::fromRgbF(from.redF() * keep + to.redF() * amount, from.greenF() * keep + to.greenF() * amount,
                            from.blueF() * keep + to.blueF() * amount, from.alphaF() * keep + to.alphaF() * amount);
}

} // namespace theme
