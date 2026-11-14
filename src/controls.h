#pragma once

#include <QAbstractButton>
#include <QPushButton>
#include <QStringList>
#include <QWidget>

class QHBoxLayout;
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
    // The parent draws the checked state, for example a selection that slides between buttons.
    void setCheckedDrawnByParent(bool value);

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
    bool checkedDrawnByParent_ = false;
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
    QString currentText() const { return labels_.value(current_); }
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

// A floating panel that sits above the content. Its layout already accounts for the shadow.
class Island : public QWidget {
    Q_OBJECT

public:
    explicit Island(QWidget *parent = nullptr);

    static constexpr int shadowMargin = 12;

    QHBoxLayout *row() const { return row_; }
    void addSeparator();
    // The panel without its shadow, in this widget's coordinates.
    QRectF panel() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    // Painted between the panel background and its children.
    virtual void paintPanel(QPainter &) {}

private:
    QHBoxLayout *row_;
};

// A message pill. It can stay up as a notice or appear briefly as a toast.
class Notice final : public QWidget {
    Q_OBJECT

public:
    enum Tone { Success, Warning };

    explicit Notice(Tone tone, QWidget *parent = nullptr);

    static constexpr int shadowMargin = 8;

    void setMessage(const QString &message);
    QString message() const { return message_; }
    // Shows the message, then fades it out.
    void flash(const QString &message);
    // The size the notice needs, shadow included, when it may be at most maxWidth wide.
    QSize sizeFor(int maxWidth) const;

signals:
    void messageChanged();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    Tone tone_;
    QString message_;
    QVariantAnimation *fade_;
    QTimer *hold_;
    qreal opacity_ = 1;
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
