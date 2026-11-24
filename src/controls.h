#pragma once

#include <QAbstractButton>
#include <QPushButton>
#include <QStringList>
#include <QWidget>

#include <functional>

class QLabel;
class QTimer;
class QVariantAnimation;

// The controls every Frameline window is built from. They paint themselves from the theme
// palette, so they follow light and dark without per-widget style sheets.

enum class Icon {
    None, Region, Screen, Library, Settings, Arrow, Pen, Rectangle, Blur, Redact, Undo, Redo,
    Copy, Save, Minus, Plus, Back, Forward, Refresh, Check, Warning,
};

// Draws an icon from a 24-unit grid into rect.
void paintIcon(QPainter &painter, Icon icon, const QRectF &rect, const QColor &color, qreal stroke = 1.6);

// A short vertical hairline between groups of controls in a row.
QWidget *separator(QWidget *parent);

class Button : public QPushButton {
    Q_OBJECT

public:
    enum Kind { Primary, Tonal, Ghost };

    Button(Kind kind, Icon icon, const QString &text, QWidget *parent = nullptr);

    // A shortcut shown after the label.
    void setHint(const QString &hint);
    void setLarge(bool large);
    // Shows only the icon; the label becomes the tooltip.
    void setCompact(bool compact);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    bool showsLabel() const;
    void animateHover(qreal target);

    Kind kind_;
    Icon icon_;
    QString hint_;
    QVariantAnimation *hoverAnimation_;
    qreal hover_ = 0;
    bool large_ = false;
    bool compact_ = false;
    bool keyboardFocus_ = false;
};

// An on/off switch whose label is part of the click target.
class Toggle final : public QAbstractButton {
    Q_OBJECT

public:
    explicit Toggle(const QString &text, QWidget *parent = nullptr);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void animate();

    QVariantAnimation *animation_;
    qreal position_ = 0;
};

// A choice between a few options that are all visible at once.
class Segmented final : public QWidget {
    Q_OBJECT

public:
    explicit Segmented(const QStringList &labels, QWidget *parent = nullptr);

    int current() const { return current_; }
    void setCurrent(int index);

    QSize sizeHint() const override;

signals:
    void currentChanged(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QRectF segment(int index) const;

    QStringList labels_;
    QVariantAnimation *animation_;
    QRectF thumb_;
    int current_ = 0;
};

// A toast: a pill that confirms an action and fades out.
class Notice final : public QWidget {
    Q_OBJECT

public:
    explicit Notice(QWidget *parent = nullptr);

    static constexpr int shadowMargin = 8;

    // Shows the message, then fades it out.
    void flash(const QString &message);
    // The size the notice needs, shadow included, when it may be at most maxWidth wide.
    QSize sizeFor(int maxWidth) const;

signals:
    void messageChanged();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString message_;
    QVariantAnimation *fade_;
    QTimer *hold_;
    qreal opacity_ = 1;
};

// The strip of controls along the top of a window, with a hairline under it.
class TopBar final : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *event) override;
};

// A container whose owner lays out the children by hand.
class Frame final : public QWidget {
public:
    using QWidget::QWidget;
    std::function<void()> onResize;

protected:
    void resizeEvent(QResizeEvent *event) override;
};

// A full-width warning strip for a problem that persists, such as a shortcut another app owns.
class Banner final : public QWidget {
    Q_OBJECT

public:
    explicit Banner(QWidget *parent = nullptr);

    void setMessage(const QString &message);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QLabel *text_;
};

// A centred explanation for a view with nothing to show: empty, not set up, loading or failed.
class EmptyState final : public QWidget {
    Q_OBJECT

public:
    explicit EmptyState(QWidget *parent = nullptr);

    void set(Icon icon, const QString &title, const QString &details, const QString &action = {});
    Button *action() const { return action_; }

private:
    class Glyph;

    Glyph *glyph_;
    QLabel *title_;
    QLabel *details_;
    Button *action_;
};
