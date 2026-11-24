#include "editor.h"

#include "annotations.h"
#include "controls.h"
#include "pngexport.h"
#include "theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QShortcut>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <optional>
#include <utility>

namespace {

constexpr QMargins stageMargins(24, 24, 24, 24);

struct ToolChoice {
    Tool tool;
    Icon icon;
    const char *label;
    const char *key;
};

constexpr std::array toolChoices{
    ToolChoice{Tool::Arrow, Icon::Arrow, "Arrow", "A"},
    ToolChoice{Tool::Pen, Icon::Pen, "Pen", "P"},
    ToolChoice{Tool::Rectangle, Icon::Rectangle, "Rectangle", "R"},
    ToolChoice{Tool::Blur, Icon::Blur, "Blur", "B"},
    ToolChoice{Tool::Redact, Icon::Redact, "Redact", "X"},
};

constexpr std::array markColors{
    std::pair{"Red", 0xff5373}, std::pair{"Orange", 0xff9e4b}, std::pair{"Yellow", 0xffdb6a},
    std::pair{"Green", 0x60d9a2}, std::pair{"Blue", 0x6db6ff}, std::pair{"Purple", 0xb58aff},
    std::pair{"White", 0xffffff}, std::pair{"Black", 0x111420},
};

constexpr std::array strokeWidths{2, 4, 6, 10, 16};
constexpr int defaultStrokeIndex = 2;

QString nativeKeys(QKeySequence::StandardKey key)
{
    return QKeySequence(key).toString(QKeySequence::NativeText);
}

QString withKeys(const QString &label, const QString &keys)
{
    return keys.isEmpty() ? label : label + QStringLiteral("  ·  ") + keys;
}

void paintChecker(QPainter &painter, const QRect &area)
{
    const theme::Palette &c = theme::colors();
    const QColor light = c.raised;
    const QColor dark = c.control;
    painter.fillRect(area, light);
    constexpr int tile = 8;
    for (int y = area.top() / tile * tile; y <= area.bottom(); y += tile) {
        for (int x = area.left() / tile * tile; x <= area.right(); x += tile) {
            if ((x / tile + y / tile) % 2 == 0) {
                painter.fillRect(QRect(x, y, tile, tile).intersected(area), dark);
            }
        }
    }
}

// Converts a trackpad pinch into a zoom factor. Returns 0 for other events.
qreal pinchFactor(QEvent *event)
{
    if (event->type() != QEvent::NativeGesture) {
        return 0;
    }
    const auto *gesture = static_cast<QNativeGestureEvent *>(event);
    if (gesture->gestureType() != Qt::ZoomNativeGesture) {
        return 0;
    }
    return 1 + gesture->value();
}

class ScreenshotCanvas final : public QWidget {
public:
    explicit ScreenshotCanvas(QWidget *parent) : QWidget(parent)
    {
        setCursor(Qt::CrossCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAccessibleName(QStringLiteral("Screenshot"));
    }

    QImage image;
    bool opaque = false;
    Tool tool = Tool::Arrow;
    QColor color = QColor(QRgb(markColors[0].second));
    qreal strokeWidth = strokeWidths[defaultStrokeIndex];
    std::function<void(Annotation)> onCommit;
    std::function<void(qreal)> onZoomBy;

    void cancelStroke()
    {
        if (draft) {
            const QRect damage = draftDamage();
            draft.reset();
            update(damage);
        }
    }

    void updatePixels(const QRect &area)
    {
        update(widgetDamage(area));
    }

protected:
    bool event(QEvent *event) override
    {
        if (const qreal factor = pinchFactor(event); factor > 0 && onZoomBy) {
            onZoomBy(factor);
            return true;
        }
        return QWidget::event(event);
    }

    void hideEvent(QHideEvent *event) override
    {
        cancelStroke();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        if (image.isNull()) {
            painter.fillRect(event->rect(), theme::colors().stage);
            return;
        }
        if (!opaque) {
            paintChecker(painter, event->rect());
        }
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.scale(qreal(width()) / image.width(), qreal(height()) / image.height());
        painter.drawImage(QPointF(0, 0), image);
        if (!draft) {
            return;
        }

        if (draft->tool == Tool::Blur) {
            const QColor accent = theme::overlayAccent;
            QPen outline(accent);
            outline.setCosmetic(true);
            outline.setWidthF(1.5);
            outline.setStyle(Qt::DashLine);
            painter.setPen(outline);
            painter.setBrush(theme::withAlpha(accent, 0.22));
            painter.drawRect(draft->bounds());
        } else {
            paintAnnotation(painter, *draft);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || image.isNull()) {
            return;
        }
        const QColor markColor = tool == Tool::Redact ? QColor(0x13, 0x15, 0x22) : color;
        draft = Annotation{tool, markColor, strokeWidth, {imagePoint(event->position())}};
        update(draftDamage());
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (draft) {
            const QRect previous = draftDamage();
            extendStroke(event->position());
            update(previous.united(draftDamage()));
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !draft) {
            return;
        }
        const QRect previous = draftDamage();
        extendStroke(event->position());
        auto annotation = std::exchange(draft, std::nullopt);
        if (annotation->hasMark() && onCommit) {
            onCommit(std::move(*annotation));
        }
        update(previous);
    }

private:
    std::optional<Annotation> draft;

    QRect widgetDamage(const QRect &area) const
    {
        if (image.isNull() || area.isEmpty()) {
            return {};
        }
        const qreal scaleX = qreal(width()) / image.width();
        const qreal scaleY = qreal(height()) / image.height();
        const QRectF mapped(area.x() * scaleX, area.y() * scaleY, area.width() * scaleX, area.height() * scaleY);
        return mapped.toAlignedRect().adjusted(-2, -2, 2, 2);
    }

    QRect draftDamage() const
    {
        return widgetDamage(draft->affectedPixels());
    }

    QPointF imagePoint(const QPointF &point) const
    {
        return QPointF(std::clamp(point.x() / width(), 0.0, 1.0) * image.width(),
                       std::clamp(point.y() / height(), 0.0, 1.0) * image.height());
    }

    void extendStroke(const QPointF &point)
    {
        const QPointF mapped = imagePoint(point);
        if (draft->points.last() == mapped) {
            return;
        }
        if (draft->tool == Tool::Pen || draft->points.size() == 1) {
            draft->points.push_back(mapped);
        } else {
            draft->points.last() = mapped;
        }
    }
};

// The scrollable stage paints the backdrop and a hairline around the screenshot.
class Workspace final : public QScrollArea {
public:
    explicit Workspace(QWidget *parent) : QScrollArea(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setAlignment(Qt::AlignCenter);
    }

    QWidget *subject = nullptr;
    std::function<void()> onResize;
    std::function<void(qreal)> onZoomBy;

protected:
    bool viewportEvent(QEvent *event) override
    {
        if (const qreal factor = pinchFactor(event); factor > 0 && onZoomBy) {
            onZoomBy(factor);
            return true;
        }
        if (event->type() != QEvent::Paint) {
            return QScrollArea::viewportEvent(event);
        }
        const theme::Palette &c = theme::colors();
        const QRect damage = static_cast<QPaintEvent *>(event)->rect();
        QPainter painter(viewport());
        painter.fillRect(damage, c.stage);
        if (subject && subject->isVisible()) {
            const QRect frame(subject->mapTo(viewport(), QPoint(0, 0)), subject->size());
            painter.setPen(c.borderStrong);
            painter.drawRect(frame.adjusted(-1, -1, 0, 0));
        }
        return true;
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QScrollArea::resizeEvent(event);
        if (onResize) {
            onResize();
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const int delta = event->angleDelta().y();
        if (onZoomBy && (event->modifiers() & Qt::ControlModifier) && delta != 0) {
            onZoomBy(std::pow(1.2, delta / 120.0));
            event->accept();
            return;
        }
        QScrollArea::wheelEvent(event);
    }
};

class Brand final : public QWidget {
public:
    explicit Brand(QWidget *parent) : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setAccessibleName(QStringLiteral("Frameline"));
    }

    QSize sizeHint() const override
    {
        const int name = QFontMetrics(theme::font(14, theme::Weight::Semibold)).horizontalAdvance(text());
        return QSize(31 + name, theme::controlHeight);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const theme::Palette &c = theme::colors();
        QPainter painter(this);
        const QIcon logo = QApplication::windowIcon();
        const QRect mark(0, (height() - 22) / 2, 22, 22);
        if (logo.isNull())
            paintIcon(painter, Icon::Region, mark, c.text, 2);
        else
            logo.paint(&painter, mark);
        painter.setPen(c.text);
        painter.setFont(theme::font(14, theme::Weight::Semibold));
        painter.drawText(QRectF(31, 0, width() - 31, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
    }

private:
    static QString text() { return QStringLiteral("Frameline"); }
};

// The empty editor uses the same viewfinder corners as region selection.
class LauncherPage final : public QWidget {
public:
    using QWidget::QWidget;
    QWidget *content = nullptr;

protected:
    void paintEvent(QPaintEvent *) override
    {
        const theme::Palette &c = theme::colors();
        QPainter painter(this);
        painter.fillRect(rect(), c.stage);
        if (!content) {
            return;
        }
        const QRectF frame = QRectF(content->geometry()).adjusted(-52, -46, 52, 46);
        theme::paintBrackets(painter, frame, 30, 2.5, theme::withAlpha(c.accent, 0.9));
    }
};

class Swatch final : public QAbstractButton {
public:
    Swatch(const QString &name, const QColor &color, QWidget *parent) : QAbstractButton(parent), color(color)
    {
        setCheckable(true);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::TabFocus);
        setToolTip(name);
        setAccessibleName(name);
    }

    QColor color;
    QSize sizeHint() const override { return QSize(26, theme::controlHeight); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const theme::Palette &c = theme::colors();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (!isEnabled()) {
            painter.setOpacity(0.28);
        }
        const QRectF frame = QRectF(rect()).adjusted(1, 4, -1, -4);
        const QPointF center = frame.center();
        if (isEnabled() && (underMouse() || isDown())) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(isDown() ? c.controlPressed : c.controlHover);
            painter.drawRoundedRect(frame, 5, 5);
        }
        if (isChecked()) {
            painter.setPen(QPen(c.text, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, 10, 10);
        }
        painter.setPen(QPen(theme::withAlpha(c.text, 0.35), 1));
        painter.setBrush(color);
        painter.drawEllipse(center, 7, 7);
        if (hasFocus()) {
            theme::paintFocusRing(painter, frame, 5);
        }
    }
};

class StrokeChoice final : public QAbstractButton {
public:
    StrokeChoice(int pixels, QWidget *parent) : QAbstractButton(parent), pixels(pixels)
    {
        setCheckable(true);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::TabFocus);
        setToolTip(QStringLiteral("%1 px stroke").arg(pixels));
        setAccessibleName(toolTip());
    }

    int pixels;
    QSize sizeHint() const override { return QSize(28, theme::controlHeight); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const theme::Palette &c = theme::colors();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (!isEnabled()) {
            painter.setOpacity(0.28);
        }
        const QRectF frame = QRectF(rect()).adjusted(1, 4, -1, -4);
        if (isChecked()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(c.accentSoft);
            painter.drawRoundedRect(frame, 5, 5);
        } else if (underMouse() && isEnabled()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(isDown() ? c.controlPressed : c.controlHover);
            painter.drawRoundedRect(frame, 5, 5);
        }
        const QPointF center = frame.center();
        painter.setPen(QPen(isChecked() ? c.accent : c.textMuted, 1 + pixels * 0.4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(center - QPointF(6, 0), center + QPointF(6, 0));
        if (hasFocus()) {
            theme::paintFocusRing(painter, frame, 5);
        }
    }
};

constexpr auto editorRules = R"(
    QLabel#hero { font-size: 30px; font-weight: 600; }
    QWidget#toolRail { background: @surface; border-right: 1px solid @border; }
    QWidget#editorFooter { background: @surface; border-top: 1px solid @border; }
)";

struct UndoPatch {
    qsizetype index;
    QPoint origin;
    QImage pixels;

    QRect restore(QImage &image) const
    {
        // The working image stays in the same 32-bit format across an editing session.
        for (int y = 0; y < pixels.height(); ++y) {
            auto *out = image.scanLine(origin.y() + y) + origin.x() * sizeof(QRgb);
            std::memcpy(out, pixels.constScanLine(y), pixels.width() * sizeof(QRgb));
        }
        return QRect(origin, pixels.size());
    }
};

} // namespace

struct EditorWindow::Ui {
    explicit Ui(EditorWindow &window) : window(window)
    {
        window.setWindowTitle(QStringLiteral("Frameline[*]"));
        window.setMinimumSize(720, 500);
        window.resize(1180, 780);
        root = new QWidget(&window);
        root->setObjectName(QStringLiteral("surface"));
        root->setAttribute(Qt::WA_StyledBackground);
        theme::attach(root, editorRules);
        window.setCentralWidget(root);

        auto *column = new QVBoxLayout(root);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        column->addWidget(makeTopBar());
        shortcutBanner = new Banner(root);
        libraryBanner = new Banner(root);
        column->addWidget(shortcutBanner);
        column->addWidget(libraryBanner);

        stage = new Frame(root);
        column->addWidget(stage, 1);
        pages = new QStackedWidget(stage);
        pages->addWidget(makeLauncher());
        pages->addWidget(makeEditor());
        toast = new Notice(stage);
        QObject::connect(toast, &Notice::messageChanged, &window, [this] { layoutStage(); });
        stage->onResize = [this] { layoutStage(); };

        addShortcuts();
        selectTool(Tool::Arrow);
    }

    EditorWindow &window;
    QWidget *root = nullptr;
    Frame *stage = nullptr;
    QStackedWidget *pages = nullptr;
    Banner *shortcutBanner = nullptr;
    Banner *libraryBanner = nullptr;
    Notice *toast = nullptr;
    Button *regionButton = nullptr;
    Button *screenButton = nullptr;
    Button *libraryButton = nullptr;
    Button *undo = nullptr;
    Button *redo = nullptr;
    QWidget *editSeparator = nullptr;
    Button *copy = nullptr;
    Button *save = nullptr;
    Button *heroRegion = nullptr;
    Button *heroScreen = nullptr;
    QLabel *shortcutTip = nullptr;
    Workspace *workspace = nullptr;
    QWidget *holder = nullptr;
    ScreenshotCanvas *canvas = nullptr;
    QLabel *dimensions = nullptr;
    Button *zoomButton = nullptr;
    QButtonGroup *tools = nullptr;
    QList<Swatch *> swatches;
    QList<StrokeChoice *> strokes;
    QImage source;
    QVector<Annotation> annotations;
    std::deque<UndoPatch> undoPatches;
    qsizetype undoBytes = 0;
    qsizetype historyCount = 0;
    qsizetype cleanCount = 0;
    qreal zoom = 1;
    bool fit = true;
    QMetaObject::Connection screenChangedConnection;

    QWidget *makeTopBar()
    {
        auto *bar = new TopBar(root);
        auto *row = new QHBoxLayout(bar);
        row->setContentsMargins(16, 8, 12, 8);
        row->setSpacing(4);
        row->addWidget(new Brand(bar));
        row->addSpacing(12);

        regionButton = new Button(Button::Ghost, Icon::Region, QStringLiteral("Region"), bar);
        screenButton = new Button(Button::Ghost, Icon::Screen, QStringLiteral("Screen"), bar);
        libraryButton = new Button(Button::Ghost, Icon::Library, QStringLiteral("Library"), bar);
        regionButton->setAccessibleName(QStringLiteral("Capture a region"));
        screenButton->setAccessibleName(QStringLiteral("Capture the screen"));
        regionButton->setToolTip(regionButton->accessibleName());
        screenButton->setToolTip(screenButton->accessibleName());
        libraryButton->setToolTip(QStringLiteral("Browse screenshots"));
        row->addWidget(regionButton);
        row->addWidget(screenButton);
        row->addWidget(libraryButton);
        row->addStretch();
        QObject::connect(regionButton, &QPushButton::clicked, &window, &EditorWindow::regionCaptureRequested);
        QObject::connect(screenButton, &QPushButton::clicked, &window, &EditorWindow::fullCaptureRequested);
        QObject::connect(libraryButton, &QPushButton::clicked, &window, &EditorWindow::libraryRequested);

        undo = new Button(Button::Ghost, Icon::Undo, {}, bar);
        redo = new Button(Button::Ghost, Icon::Redo, {}, bar);
        undo->setToolTip(withKeys(QStringLiteral("Undo"), nativeKeys(QKeySequence::Undo)));
        redo->setToolTip(withKeys(QStringLiteral("Redo"), nativeKeys(QKeySequence::Redo)));
        undo->setAccessibleName(QStringLiteral("Undo"));
        redo->setAccessibleName(QStringLiteral("Redo"));
        QObject::connect(undo, &QPushButton::clicked, &window, [this] { moveHistory(-1); });
        QObject::connect(redo, &QPushButton::clicked, &window, [this] { moveHistory(1); });
        row->addWidget(undo);
        row->addWidget(redo);
        editSeparator = separator(bar);
        row->addWidget(editSeparator);

        copy = new Button(Button::Tonal, Icon::Copy, QStringLiteral("Copy"), bar);
        save = new Button(Button::Primary, Icon::Save, QStringLiteral("Save"), bar);
        copy->setToolTip(withKeys(QStringLiteral("Copy to clipboard"), nativeKeys(QKeySequence::Copy)));
        save->setToolTip(withKeys(QStringLiteral("Save as PNG"), nativeKeys(QKeySequence::Save)));
        QObject::connect(copy, &QPushButton::clicked, &window, [this] { copyImage(); });
        QObject::connect(save, &QPushButton::clicked, &window, [this] { saveImage(); });
        row->addWidget(copy);
        row->addSpacing(4);
        row->addWidget(save);
        row->addSpacing(4);

        auto *settings = new Button(Button::Ghost, Icon::Settings, {}, bar);
        settings->setToolTip(QStringLiteral("Settings"));
        settings->setAccessibleName(QStringLiteral("Settings"));
        QObject::connect(settings, &QPushButton::clicked, &window, &EditorWindow::settingsRequested);
        row->addWidget(settings);
        return bar;
    }

    QWidget *makeLauncher()
    {
        auto *page = new LauncherPage(pages);
        auto *column = new QVBoxLayout(page);
        column->setContentsMargins(24, 24, 24, 24);
        column->addStretch(3);

        auto *content = new QWidget(page);
        content->setFixedWidth(560);
        auto *stack = new QVBoxLayout(content);
        stack->setContentsMargins(0, 0, 0, 0);
        stack->setSpacing(0);
        auto *title = theme::label(QStringLiteral("Take a screenshot"), "hero", content);
        title->setAlignment(Qt::AlignCenter);
        stack->addWidget(title);
        stack->addSpacing(24);

        heroRegion = new Button(Button::Primary, Icon::Region, QStringLiteral("Select region"), content);
        heroScreen = new Button(Button::Tonal, Icon::Screen, QStringLiteral("Capture screen"), content);
        heroRegion->setAccessibleName(QStringLiteral("Select screen region"));
        heroScreen->setAccessibleName(QStringLiteral("Capture full screen"));
        heroRegion->setLarge(true);
        heroScreen->setLarge(true);
        QObject::connect(heroRegion, &QPushButton::clicked, &window, &EditorWindow::regionCaptureRequested);
        QObject::connect(heroScreen, &QPushButton::clicked, &window, &EditorWindow::fullCaptureRequested);
        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(10);
        buttons->addStretch();
        buttons->addWidget(heroRegion);
        buttons->addWidget(heroScreen);
        buttons->addStretch();
        stack->addLayout(buttons);
        stack->addSpacing(22);

#ifdef Q_OS_MACOS
        const QString tip = QStringLiteral("The shortcuts work in any app while Frameline is in the menu bar.");
#else
        const QString tip = QStringLiteral("The shortcuts work in any app while Frameline is running.");
#endif
        shortcutTip = theme::label(tip, "caption", content);
        shortcutTip->setAlignment(Qt::AlignCenter);
        shortcutTip->setWordWrap(true);
        shortcutTip->hide();
        stack->addWidget(shortcutTip);

        auto *centered = new QHBoxLayout;
        centered->addStretch();
        centered->addWidget(content);
        centered->addStretch();
        column->addLayout(centered);
        column->addStretch(4);
        page->content = content;
        return page;
    }

    QWidget *makeEditor()
    {
        auto *page = new QWidget(pages);
        auto *column = new QVBoxLayout(page);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);

        workspace = new Workspace(page);
        holder = new QWidget;
        canvas = new ScreenshotCanvas(holder);
        workspace->setWidget(holder);
        holder->setAutoFillBackground(false);
        workspace->subject = canvas;
        workspace->onResize = [this] {
            if (fit) {
                updateZoom();
            }
        };
        const auto zoomBy = [this](qreal factor) { setZoom(zoom * factor); };
        workspace->onZoomBy = zoomBy;
        canvas->onZoomBy = zoomBy;
        canvas->onCommit = [this](Annotation annotation) {
            if (cleanCount > historyCount) {
                cleanCount = -1;
            }
            annotations.resize(historyCount);
            annotations.push_back(std::move(annotation));
            applyNextAnnotation();
        };

        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        row->addWidget(makeToolRail(page));
        row->addWidget(workspace, 1);
        column->addLayout(row, 1);
        column->addWidget(makeEditorFooter(page));
        return page;
    }

    QWidget *makeToolRail(QWidget *parent)
    {
        auto *rail = new QWidget(parent);
        rail->setObjectName(QStringLiteral("toolRail"));
        rail->setAttribute(Qt::WA_StyledBackground);
        auto *column = new QVBoxLayout(rail);
        column->setContentsMargins(8, 10, 8, 8);
        column->setSpacing(4);
        tools = new QButtonGroup(rail);
        for (const auto &choice : toolChoices) {
            const QString label = QString::fromUtf8(choice.label);
            const QString key = QString::fromLatin1(choice.key);
            auto *button = new Button(Button::Ghost, choice.icon, {}, rail);
            button->setCheckable(true);
            button->setLarge(true);
            button->setToolTip(withKeys(label, key));
            button->setAccessibleName(label);
            tools->addButton(button, static_cast<int>(choice.tool));
            column->addWidget(button);
            addShortcut(QKeySequence(key), [this, button] {
                if (!source.isNull()) {
                    button->click();
                }
            });
        }
        column->addStretch();
        QObject::connect(tools, &QButtonGroup::idClicked, &window, [this](int id) { selectTool(static_cast<Tool>(id)); });
        return rail;
    }

    QWidget *makeEditorFooter(QWidget *parent)
    {
        auto *footer = new QWidget(parent);
        footer->setObjectName(QStringLiteral("editorFooter"));
        footer->setAttribute(Qt::WA_StyledBackground);
        auto *row = new QHBoxLayout(footer);
        row->setContentsMargins(12, 5, 12, 5);
        row->setSpacing(2);

        auto *colorGroup = new QButtonGroup(footer);
        for (const auto &[name, value] : markColors) {
            auto *swatch = new Swatch(QString::fromLatin1(name), QColor(QRgb(value)), footer);
            colorGroup->addButton(swatch);
            swatches.append(swatch);
            row->addWidget(swatch);
            QObject::connect(swatch, &QAbstractButton::clicked, &window, [this, swatch] { canvas->color = swatch->color; });
        }
        swatches.first()->setChecked(true);

        row->addWidget(separator(footer));
        auto *strokeGroup = new QButtonGroup(footer);
        for (const int pixels : strokeWidths) {
            auto *choice = new StrokeChoice(pixels, footer);
            strokeGroup->addButton(choice);
            strokes.append(choice);
            row->addWidget(choice);
            QObject::connect(choice, &QAbstractButton::clicked, &window, [this, pixels] { canvas->strokeWidth = pixels; });
        }
        strokes.at(defaultStrokeIndex)->setChecked(true);

        row->addStretch();
        dimensions = theme::label({}, "caption", footer);
        dimensions->setFont(theme::font(12, theme::Weight::Regular, true));
        dimensions->setToolTip(QStringLiteral("Screenshot size in pixels"));
        row->addWidget(dimensions);
        row->addWidget(separator(footer));
        auto *minus = new Button(Button::Ghost, Icon::Minus, {}, footer);
        auto *plus = new Button(Button::Ghost, Icon::Plus, {}, footer);
        minus->setToolTip(withKeys(QStringLiteral("Zoom out"), QStringLiteral("−")));
        plus->setToolTip(withKeys(QStringLiteral("Zoom in"), QStringLiteral("+")));
        minus->setAccessibleName(QStringLiteral("Zoom out"));
        plus->setAccessibleName(QStringLiteral("Zoom in"));
        zoomButton = new Button(Button::Ghost, Icon::None, QStringLiteral("100%"), footer);
        zoomButton->setFixedWidth(60);
        QObject::connect(minus, &QPushButton::clicked, &window, [this] { stepZoom(-1); });
        QObject::connect(plus, &QPushButton::clicked, &window, [this] { stepZoom(1); });
        QObject::connect(zoomButton, &QPushButton::clicked, &window, [this] {
            if (fit && std::abs(zoom - 1) > 0.001) {
                setZoom(1);
            } else {
                setFit();
            }
        });
        row->addWidget(minus);
        row->addWidget(zoomButton);
        row->addWidget(plus);
        return footer;
    }

    void layoutStage()
    {
        pages->setGeometry(stage->rect());
        const int width = stage->width();
        for (Button *button : {regionButton, screenButton, libraryButton}) {
            button->setCompact(width < 840);
        }
        if (toast->isVisible()) {
            const QSize size = toast->sizeFor(std::min(width - 32, 520));
            toast->setGeometry((width - size.width()) / 2, 14 - Notice::shadowMargin, size.width(), size.height());
            toast->raise();
        }
    }

    void addShortcuts()
    {
        addShortcut(QKeySequence::Undo, [this] { moveHistory(-1); });
        addShortcut(QKeySequence::Redo, [this] { moveHistory(1); });
        addShortcut(QKeySequence::Save, [this] { saveImage(); });
        addShortcut(QKeySequence::Copy, [this] { copyImage(); });
        addShortcut(QKeySequence(Qt::Key_Escape), [this] { canvas->cancelStroke(); });
        addShortcut(QKeySequence(Qt::Key_0), [this] { setFit(); });
        addShortcut(QKeySequence(Qt::Key_1), [this] { setZoom(1); });
        addShortcut(QKeySequence(Qt::Key_Plus), [this] { stepZoom(1); });
        addShortcut(QKeySequence(Qt::Key_Equal), [this] { stepZoom(1); });
        addShortcut(QKeySequence(Qt::Key_Minus), [this] { stepZoom(-1); });
    }

    void addShortcut(const QKeySequence &sequence, const std::function<void()> &handler)
    {
        auto *shortcut = new QShortcut(sequence, &window);
        QObject::connect(shortcut, &QShortcut::activated, &window, handler);
    }

    void selectTool(Tool tool)
    {
        canvas->cancelStroke();
        canvas->tool = tool;
        tools->button(static_cast<int>(tool))->setChecked(true);
        refreshControls();
    }

    void refreshControls()
    {
        const bool hasImage = !source.isNull();
        for (QWidget *control : {static_cast<QWidget *>(regionButton), static_cast<QWidget *>(screenButton),
                                 static_cast<QWidget *>(undo), static_cast<QWidget *>(redo), editSeparator,
                                 static_cast<QWidget *>(copy), static_cast<QWidget *>(save)}) {
            control->setVisible(hasImage);
        }
        undo->setEnabled(historyCount > 0);
        redo->setEnabled(historyCount < annotations.size());
        const bool drawsLine = canvas->tool != Tool::Blur && canvas->tool != Tool::Redact;
        const QString unavailable = canvas->tool == Tool::Blur ? QStringLiteral("Blur has no color or stroke")
                                                               : QStringLiteral("Redact always uses a solid block");
        for (Swatch *swatch : swatches) {
            swatch->setEnabled(drawsLine);
            swatch->setToolTip(drawsLine ? swatch->accessibleName() : unavailable);
        }
        for (StrokeChoice *choice : strokes) {
            choice->setEnabled(drawsLine);
            choice->setToolTip(drawsLine ? choice->accessibleName() : unavailable);
        }
        window.setWindowModified(hasImage && historyCount != cleanCount);
    }

    void setCapture(QImage image)
    {
        image.setDevicePixelRatio(1);
        source = std::move(image);
        canvas->opaque = !source.hasAlphaChannel();
        annotations.clear();
        undoPatches.clear();
        undoBytes = 0;
        historyCount = 0;
        cleanCount = 0;
        fit = true;
        canvas->cancelStroke();
        pages->setCurrentIndex(source.isNull() ? 0 : 1);
        dimensions->setText(QStringLiteral("%1 × %2 px").arg(source.width()).arg(source.height()));
        updateImage();
        updateZoom();
        if (!source.isNull()) {
            canvas->setFocus();
        }
    }

    void moveHistory(int step)
    {
        canvas->cancelStroke();
        if (step < 0 && historyCount > 0) {
            --historyCount;
            if (!undoPatches.empty() && undoPatches.back().index == historyCount) {
                const UndoPatch &patch = undoPatches.back();
                canvas->updatePixels(patch.restore(canvas->image));
                undoBytes -= patch.pixels.sizeInBytes();
                undoPatches.pop_back();
                refreshControls();
            } else {
                updateImage();
            }
        } else if (step > 0 && historyCount < annotations.size()) {
            applyNextAnnotation();
        }
    }

    void applyNextAnnotation()
    {
        // Keep recent undos cheap without retaining a full-size image per edit.
        // Older states are reconstructed from the original and the vector annotations.
        constexpr qsizetype budget = 64 * 1024 * 1024;
        constexpr size_t maxPatches = 128;
        const Annotation &annotation = annotations.at(historyCount);
        const QImage::Format format = canvas->opaque ? QImage::Format_RGB32 : QImage::Format_ARGB32_Premultiplied;
        if (canvas->image.format() != format) {
            canvas->image.convertTo(format);
        }
        const QRect area = annotation.affectedPixels().intersected(canvas->image.rect());
        const qint64 cost = qint64(area.width()) * area.height() * sizeof(QRgb);
        if (cost > 0 && cost <= budget) {
            while (!undoPatches.empty() && (undoBytes + cost > budget || undoPatches.size() >= maxPatches)) {
                undoBytes -= undoPatches.front().pixels.sizeInBytes();
                undoPatches.pop_front();
            }
            QImage pixels = canvas->image.copy(area);
            if (!pixels.isNull()) {
                undoBytes += pixels.sizeInBytes();
                undoPatches.push_back({historyCount, area.topLeft(), std::move(pixels)});
            }
        }
        applyAnnotation(canvas->image, annotation);
        ++historyCount;
        canvas->updatePixels(area);
        refreshControls();
    }

    void updateImage()
    {
        canvas->image = renderAnnotations(source, annotations, historyCount);
        canvas->update();
        refreshControls();
    }

    void setFit()
    {
        fit = true;
        updateZoom();
    }

    void stepZoom(int direction)
    {
        setZoom(zoom * std::pow(1.25, direction));
    }

    void setZoom(qreal value)
    {
        if (source.isNull()) {
            return;
        }
        fit = false;
        zoom = std::clamp(value, 0.025, 8.0);
        updateZoom();
    }

    void updateZoom()
    {
        if (source.isNull()) {
            return;
        }
        const qreal dpr = workspace->devicePixelRatioF();
        const QMargins &m = stageMargins;
        if (fit) {
            const QSize available = workspace->maximumViewportSize()
                - QSize(m.left() + m.right(), m.top() + m.bottom());
            zoom = std::min({1.0, qreal(std::max(1, available.width())) * dpr / source.width(),
                             qreal(std::max(1, available.height())) * dpr / source.height()});
        }
        const QSize size(std::max(1, qRound(source.width() * zoom / dpr)),
                         std::max(1, qRound(source.height() * zoom / dpr)));
        const QPoint viewCenter(workspace->viewport()->width() / 2, workspace->viewport()->height() / 2);
        const QPoint oldCenter = canvas->mapFrom(workspace->viewport(), viewCenter);
        const QPointF relative(qreal(oldCenter.x()) / std::max(1, canvas->width()),
                               qreal(oldCenter.y()) / std::max(1, canvas->height()));
        holder->setFixedSize(size + QSize(m.left() + m.right(), m.top() + m.bottom()));
        canvas->setGeometry(QRect(QPoint(m.left(), m.top()), size));
        if (!fit) {
            workspace->horizontalScrollBar()->setValue(qRound(m.left() + relative.x() * size.width()) - viewCenter.x());
            workspace->verticalScrollBar()->setValue(qRound(m.top() + relative.y() * size.height()) - viewCenter.y());
        }
        workspace->viewport()->update();
        zoomButton->setText(QStringLiteral("%1%").arg(qRound(zoom * 100)));
        zoomButton->setToolTip(fit ? QStringLiteral("Fitted to the window. Click for actual size  ·  1")
                                   : QStringLiteral("Click to fit the window  ·  0"));
    }

    void copyImage()
    {
        if (canvas->image.isNull()) {
            return;
        }
        QApplication::clipboard()->setImage(canvas->image);
        cleanCount = historyCount;
        refreshControls();
        toast->flash(QStringLiteral("Copied to clipboard"));
    }

    void saveImage()
    {
        if (canvas->image.isNull() || !saveImageAs(&window, canvas->image, QDateTime::currentDateTime())) {
            return;
        }
        cleanCount = historyCount;
        refreshControls();
        toast->flash(QStringLiteral("Screenshot saved"));
    }
};

EditorWindow::EditorWindow(QWidget *parent) : QMainWindow(parent), ui(std::make_unique<Ui>(*this)) {}

EditorWindow::~EditorWindow() = default;

void EditorWindow::closeEvent(QCloseEvent *event)
{
    QMainWindow::closeEvent(event);
    if (event->isAccepted()) {
        emit closeAccepted();
    }
}

void EditorWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (windowHandle()) {
        QObject::disconnect(ui->screenChangedConnection);
        ui->screenChangedConnection = QObject::connect(windowHandle(), &QWindow::screenChanged, this,
                                                        [this] { ui->updateZoom(); });
    }
    ui->updateZoom();
}

void EditorWindow::setCapture(const QImage &image)
{
    ui->setCapture(image);
}

QImage EditorWindow::currentImage() const
{
    return ui->canvas->image;
}

void EditorWindow::setShortcuts(const QString &full, const QString &region, const QString &warning)
{
    ui->heroRegion->setHint(region);
    ui->heroScreen->setHint(full);
    ui->regionButton->setToolTip(withKeys(QStringLiteral("Capture a region"), region));
    ui->screenButton->setToolTip(withKeys(QStringLiteral("Capture the screen"), full));
    ui->shortcutTip->setVisible(!full.isEmpty() || !region.isEmpty());
    ui->shortcutBanner->setMessage(warning);
}

void EditorWindow::setLibraryStatus(const QString &message)
{
    ui->libraryBanner->setMessage(message);
}

bool EditorWindow::hasUnsavedChanges() const
{
    return !ui->source.isNull() && ui->cleanCount != ui->historyCount;
}

bool EditorWindow::confirmReplace()
{
    if (!hasUnsavedChanges()) {
        return true;
    }
    QMessageBox dialog(QMessageBox::Warning, QStringLiteral("Discard edits?"), QStringLiteral("Discard your edits?"),
                       QMessageBox::NoButton, this);
    dialog.setInformativeText(QStringLiteral("The markup on this screenshot hasn’t been copied or saved."));
    auto *discard = dialog.addButton(QStringLiteral("Discard"), QMessageBox::DestructiveRole);
    auto *keep = dialog.addButton(QStringLiteral("Keep Editing"), QMessageBox::RejectRole);
    dialog.setDefaultButton(keep);
    dialog.exec();
    return dialog.clickedButton() == discard;
}
