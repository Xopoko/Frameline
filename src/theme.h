#pragma once

#include <QColor>
#include <QFont>
#include <QObject>
#include <QRectF>
#include <QString>

class QLabel;
class QPainter;
class QWidget;

// FreezeCut's design tokens: the light and dark palettes, the type scale, the style sheet for
// standard controls, and the painting primitives shared by every window.
namespace theme {

struct Palette {
    bool dark;
    QColor stage;          // Backdrop behind screenshots.
    QColor surface;        // Window and toolbar background.
    QColor raised;         // Floating panels and grouped rows.
    QColor inset;          // Text fields.
    QColor control;        // Resting buttons and tracks.
    QColor controlHover;
    QColor controlPressed;
    QColor border;
    QColor borderStrong;
    QColor text;
    QColor textMuted;
    QColor textFaint;      // Disabled text and decoration only.
    QColor accent;
    QColor accentHover;
    QColor onAccent;
    QColor accentSoft;     // Selection fill.
    QColor warning;
    QColor danger;
    QColor shadow;
};

const Palette &colors();

// The accent for guides drawn over arbitrary image content.
inline const QColor overlayAccent{0xa7, 0xb2, 0xff};

inline constexpr int radius = 8;
inline constexpr int controlHeight = 32;

class Notifier final : public QObject {
    Q_OBJECT

signals:
    void changed();
};

// Emits changed() after the system switches between light and dark.
Notifier *notifier();

// Applies the shared style sheet, followed by the window's own rules, and keeps it current.
// Rules may refer to palette entries as @name, for example "background: @surface;".
void attach(QWidget *root, const char *windowRules = "");

enum class Weight { Regular, Medium, Semibold };

// The interface font at a pixel size. Tabular figures keep changing numbers from jittering.
QFont font(int pixelSize, Weight weight = Weight::Regular, bool tabular = false);

// Creates a plain-text label. The role is the object name the style sheet selects on. The shared
// sheet styles title, heading, muted, caption and emptyTitle; any other role keeps the base style
// unless the window's own rules name it.
QLabel *label(const QString &text, const char *role, QWidget *parent);

// A soft drop shadow around a rounded rectangle.
void paintShadow(QPainter &painter, const QRectF &rect, qreal cornerRadius, int spread, qreal offset, qreal strength);

// Viewfinder corners drawn just inside rect.
void paintBrackets(QPainter &painter, const QRectF &rect, qreal length, qreal width, const QColor &color);

void paintFocusRing(QPainter &painter, const QRectF &rect, qreal cornerRadius);

// A shortcut drawn as a single key cap. The size includes the cap's padding.
QSize keycapSize(const QString &keys);
void paintKeycap(QPainter &painter, const QRectF &rect, const QString &keys);

QColor withAlpha(QColor color, qreal alpha);
QColor mix(const QColor &from, const QColor &to, qreal amount);

} // namespace theme
