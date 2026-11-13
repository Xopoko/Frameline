#include "editor.h"

#include "annotations.h"
#include "pngexport.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPolygonF>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QToolButton>
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

using screenframe::Annotation;
using screenframe::Tool;

enum class Icon { Full, Region, Arrow, Pen, Rectangle, Blur, Redact, Undo, Redo, Copy, Save, Minus, Plus };

QIcon makeIcon(Icon icon, const QColor &color = QColor("#dce1fb"))
{
    QPixmap image(48, 48);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(2);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.65, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    auto line = [&](qreal x1, qreal y1, qreal x2, qreal y2) {
        painter.drawLine(QPointF(x1, y1), QPointF(x2, y2));
    };
    switch (icon) {
    case Icon::Full:
        painter.drawRoundedRect(QRectF(3.25, 4.5, 17.5, 13), 1.7, 1.7);
        line(9, 20.5, 15, 20.5);
        line(12, 17.5, 12, 20.5);
        break;
    case Icon::Region:
        for (const auto &point : {QPointF(4, 4), QPointF(20, 4), QPointF(4, 20), QPointF(20, 20)}) {
            line(point.x(), point.y(), point.x() == 4 ? 9 : 15, point.y());
            line(point.x(), point.y(), point.x(), point.y() == 4 ? 9 : 15);
        }
        line(9, 12, 15, 12);
        line(12, 9, 12, 15);
        break;
    case Icon::Arrow:
        line(5, 19, 19, 5);
        line(12, 5, 19, 5);
        line(19, 5, 19, 12);
        break;
    case Icon::Pen:
        painter.drawPolygon(QPolygonF{QPointF(4, 20), QPointF(5.2, 14.2), QPointF(16.5, 3),
                                      QPointF(21, 7.5), QPointF(9.8, 18.8)});
        line(14, 5.5, 18.5, 10);
        break;
    case Icon::Rectangle:
        painter.drawRoundedRect(QRectF(4, 5, 16, 14), 1.5, 1.5);
        break;
    case Icon::Blur:
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                const qreal radius = 1.25 + 0.3 * column;
                painter.drawEllipse(QPointF(6 + column * 6, 6 + row * 6), radius, radius);
            }
        }
        break;
    case Icon::Redact:
        painter.drawRoundedRect(QRectF(3.5, 5.5, 17, 13), 2, 2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(6.5, 9, 11, 6), 0.7, 0.7);
        break;
    case Icon::Redo:
        painter.translate(24, 0);
        painter.scale(-1, 1);
        [[fallthrough]];
    case Icon::Undo:
        painter.drawArc(QRectF(6, 8, 14, 13), 0, 166 * 16);
        line(4.5, 12.5, 6, 8);
        line(4.5, 12.5, 9, 13.2);
        break;
    case Icon::Copy:
        painter.drawRoundedRect(QRectF(8, 8, 12, 12), 2, 2);
        painter.drawPolyline(QPolygonF{QPointF(5, 16), QPointF(4, 16), QPointF(4, 4),
                                       QPointF(16, 4), QPointF(16, 5)});
        break;
    case Icon::Save:
        line(12, 3.5, 12, 14.5);
        line(8, 10.5, 12, 14.5);
        line(16, 10.5, 12, 14.5);
        painter.drawPolyline(QPolygonF{QPointF(4, 15), QPointF(4, 20), QPointF(20, 20), QPointF(20, 15)});
        break;
    case Icon::Plus:
        line(12, 6, 12, 18);
        [[fallthrough]];
    case Icon::Minus:
        line(6, 12, 18, 12);
        break;
    }
    painter.end();
    return QIcon(image);
}

QPushButton *makeButton(const QString &label, Icon icon, QWidget *parent, const char *style = "")
{
    auto *button = new QPushButton(makeIcon(icon), label, parent);
    button->setObjectName(QString::fromLatin1(style));
    button->setIconSize(QSize(17, 17));
    return button;
}

QToolButton *makeIconButton(Icon icon, const QString &label, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setIcon(makeIcon(icon));
    button->setIconSize(QSize(17, 17));
    button->setToolTip(label);
    button->setAccessibleName(label);
    return button;
}

QLabel *makeLabel(const QString &text, const char *style, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QString::fromLatin1(style));
    return label;
}

class ScreenshotCanvas final : public QWidget {
public:
    explicit ScreenshotCanvas(QWidget *parent = nullptr) : QWidget(parent)
    {
        setCursor(Qt::CrossCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAccessibleName(QStringLiteral("Снимок экрана"));
    }

    QImage image;
    bool opaque = false;
    Tool tool = Tool::Arrow;
    QColor color = QColor("#ff5373");
    qreal strokeWidth = 6;
    std::function<void(Annotation)> onCommit;

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
    void hideEvent(QHideEvent *event) override
    {
        cancelStroke();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        if (image.isNull() || !opaque) {
            painter.fillRect(event->rect(), QColor("#111420"));
        }
        if (image.isNull()) {
            return;
        }
        painter.scale(qreal(width()) / image.width(), qreal(height()) / image.height());
        painter.drawImage(QPointF(0, 0), image);
        if (!draft) {
            return;
        }

        if (draft->tool == Tool::Blur) {
            QPen outline(QColor("#d7c7ff"));
            outline.setCosmetic(true);
            outline.setStyle(Qt::DashLine);
            painter.setPen(outline);
            painter.setBrush(QColor(147, 105, 255, 72));
            painter.drawRect(draft->bounds());
        } else {
            screenframe::paintAnnotation(painter, *draft);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || image.isNull()) {
            return;
        }
        const QColor markColor = tool == Tool::Redact ? QColor("#131522") : color;
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
        const QRectF mapped(area.x() * scaleX, area.y() * scaleY,
                            area.width() * scaleX, area.height() * scaleY);
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

class Workspace final : public QScrollArea {
public:
    using QScrollArea::QScrollArea;
    std::function<void()> onResize;
    std::function<void(int)> onZoom;

protected:
    bool viewportEvent(QEvent *event) override
    {
        if (event->type() != QEvent::Paint) {
            return QScrollArea::viewportEvent(event);
        }
        const QRect damage = static_cast<QPaintEvent *>(event)->rect();
        QPainter painter(viewport());
        painter.fillRect(damage, QColor("#111421"));
        constexpr int tile = 16;
        for (int y = damage.top() / tile * tile; y <= damage.bottom(); y += tile) {
            for (int x = damage.left() / tile * tile; x <= damage.right(); x += tile) {
                if ((x / tile + y / tile) % 2 == 0) {
                    painter.fillRect(x, y, tile, tile, QColor("#181c2a"));
                }
            }
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
        if (onZoom && (event->modifiers() & Qt::ControlModifier) && delta != 0) {
            onZoom(delta > 0 ? 1 : -1);
            event->accept();
            return;
        }
        QScrollArea::wheelEvent(event);
    }
};

constexpr auto editorStyles = R"(
    QMainWindow, QWidget#editorRoot { background: #111421; color: #edf0ff; }
    QLabel { color: #edf0ff; }
    QWidget#header, QWidget#footer { background: #191d2d; }
    QLabel#brand { color: #f1edff; font-size: 17px; font-weight: 750; }
    QLabel#brandIcon { background: #342758; border: 1px solid #59447f; border-radius: 8px; }
    QWidget#launcher { background: #111421; }
    QWidget#launchCard { background: #1b2032; border: 1px solid #313950; border-radius: 13px; }
    QLabel#launchIcon { background: #2e244b; border-radius: 11px; }
    QLabel#launchHeading { font-size: 30px; font-weight: 750; }
    QLabel#cardHeading { font-size: 16px; font-weight: 700; }
    QLabel#section { color: #a0aaca; font-size: 12px; font-weight: 650; }
    QLabel#muted, QLabel#status { color: #b2bbd8; font-size: 12px; }
    QLabel#warning { color: #f2cb86; background: #31291e; border: 1px solid #584429; padding: 7px 12px; }
    QLabel#key { color: #dfd4ff; background: #2b2942; border: 1px solid #443960; border-radius: 4px; padding: 3px 6px; font-size: 11px; }
    QWidget#rail { background: #181c2c; border-right: 1px solid #292f44; }
    QFrame#divider { color: #343b54; background: #343b54; max-height: 1px; }
    QPushButton, QToolButton, QComboBox {
        background: #22283c; border: 1px solid #343d56; border-radius: 6px; padding: 6px 10px;
        color: #eef0ff; font-weight: 550;
    }
    QPushButton:hover, QToolButton:hover, QComboBox:hover { background: #2b334d; border-color: #58658a; }
    QPushButton:focus, QToolButton:focus, QComboBox:focus { border: 1px solid #c4a8ff; }
    QPushButton:disabled, QToolButton:disabled, QComboBox:disabled { color: #828ba8; background: #1c2032; }
    QToolButton { padding: 5px 7px; }
    QPushButton#primary { background: #7053c7; border-color: #8e6ce4; color: white; font-weight: 700; }
    QPushButton#primary:hover { background: #8261dd; }
    QPushButton#primary:focus { border-color: #f1e6ff; }
    QPushButton#tool { background: transparent; border-color: transparent; text-align: left; }
    QPushButton#tool:hover { background: #242a41; }
    QPushButton#tool:checked { background: #342752; border-color: #6c539f; color: #f6efff; }
    QPushButton#tool:focus { border-color: #c4a8ff; }
    QComboBox { padding: 4px 8px; }
    QComboBox QAbstractItemView { background: #22283c; color: #f0ecff; selection-background-color: #604a98; }
    QScrollBar:vertical { background: #181d2c; width: 12px; margin: 0; }
    QScrollBar:horizontal { background: #181d2c; height: 12px; margin: 0; }
    QScrollBar::handle { background: #475270; border-radius: 4px; min-width: 25px; min-height: 25px; }
    QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
    QScrollBar::add-page, QScrollBar::sub-page { background: none; }
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

}

struct EditorWindow::Ui {
    explicit Ui(EditorWindow &window) : window(window)
    {
        window.setWindowTitle(QStringLiteral("ScreenFrame[*]"));
        window.setMinimumSize(700, 480);
        window.resize(1080, 740);
        root = new QWidget(&window);
        root->setObjectName(QStringLiteral("editorRoot"));
        root->setStyleSheet(QString::fromUtf8(editorStyles));
        window.setCentralWidget(root);

        auto *layout = new QVBoxLayout(root);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(makeHeader());
        warning = makeLabel({}, "warning", root);
        warning->setWordWrap(true);
        warning->hide();
        layout->addWidget(warning);

        pages = new QStackedWidget(root);
        layout->addWidget(pages, 1);
        pages->addWidget(makeLauncher());
        pages->addWidget(makeEditor());
        addShortcuts();
        refreshControls();
    }

    EditorWindow &window;
    QWidget *root = nullptr;
    QStackedWidget *pages = nullptr;
    QLabel *warning = nullptr;
    QLabel *fullKey = nullptr;
    QLabel *regionKey = nullptr;
    QLabel *dimensions = nullptr;
    QLabel *status = nullptr;
    QLabel *zoomLabel = nullptr;
    QPushButton *full = nullptr;
    QPushButton *region = nullptr;
    QPushButton *save = nullptr;
    QPushButton *copy = nullptr;
    QToolButton *undo = nullptr;
    QToolButton *redo = nullptr;
    QComboBox *color = nullptr;
    QComboBox *thickness = nullptr;
    QButtonGroup *tools = nullptr;
    Workspace *workspace = nullptr;
    ScreenshotCanvas *canvas = nullptr;
    QImage source;
    QVector<Annotation> annotations;
    std::deque<UndoPatch> undoPatches;
    qsizetype undoBytes = 0;
    qsizetype historyCount = 0;
    qsizetype cleanCount = 0;
    qreal zoom = 1;
    bool fit = true;
    QMetaObject::Connection screenChangedConnection;

    QWidget *makeHeader()
    {
        auto *header = new QWidget(root);
        header->setObjectName(QStringLiteral("header"));
        auto *row = new QHBoxLayout(header);
        row->setContentsMargins(16, 11, 16, 11);
        row->setSpacing(8);
        auto *mark = makeLabel({}, "brandIcon", header);
        mark->setFixedSize(33, 33);
        mark->setAlignment(Qt::AlignCenter);
        mark->setPixmap(makeIcon(Icon::Region, QColor("#c9b0ff")).pixmap(QSize(20, 20)));
        row->addWidget(mark);
        row->addWidget(makeLabel(QStringLiteral("ScreenFrame"), "brand", header));
        row->addStretch();
        full = makeButton(QStringLiteral("Весь экран"), Icon::Full, header);
        region = makeButton(QStringLiteral("Область"), Icon::Region, header);
        row->addWidget(full);
        row->addWidget(region);
        QObject::connect(full, &QPushButton::clicked, &window, &EditorWindow::fullCaptureRequested);
        QObject::connect(region, &QPushButton::clicked, &window, &EditorWindow::regionCaptureRequested);

        copy = makeButton(QStringLiteral("Копировать"), Icon::Copy, header);
        save = makeButton(QStringLiteral("Сохранить PNG"), Icon::Save, header, "primary");
        row->addWidget(copy);
        row->addWidget(save);
        QObject::connect(copy, &QPushButton::clicked, &window, [this] { copyImage(); });
        QObject::connect(save, &QPushButton::clicked, &window, [this] { saveImage(); });
        return header;
    }

    QWidget *makeLauncher()
    {
        auto *page = new QWidget(pages);
        page->setObjectName(QStringLiteral("launcher"));
        auto *column = new QVBoxLayout(page);
        column->setContentsMargins(26, 24, 26, 24);
        column->setSpacing(12);
        column->addStretch(2);
        auto *content = new QWidget(page);
        auto *contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(0, 0, 0, 0);
        contentLayout->setSpacing(16);
        contentLayout->addWidget(makeLabel(QStringLiteral("Снимок экрана"), "launchHeading", content));

        auto *cards = new QHBoxLayout;
        cards->setSpacing(14);
        cards->addWidget(makeCaptureCard(content, true, fullKey), 1);
        cards->addWidget(makeCaptureCard(content, false, regionKey), 1);
        contentLayout->addLayout(cards);
        auto *centered = new QHBoxLayout;
        centered->setSpacing(0);
        centered->addStretch();
        content->setFixedWidth(564);
        centered->addWidget(content);
        centered->addStretch();
        column->addLayout(centered);
        column->addStretch(3);
        return page;
    }

    QWidget *makeCaptureCard(QWidget *parent, bool fullScreen, QLabel *&key)
    {
        auto *card = new QWidget(parent);
        card->setObjectName(QStringLiteral("launchCard"));
        auto *column = new QVBoxLayout(card);
        column->setContentsMargins(17, 17, 17, 16);
        column->setSpacing(8);
        const Icon icon = fullScreen ? Icon::Full : Icon::Region;
        auto *symbol = makeLabel({}, "launchIcon", card);
        symbol->setFixedSize(46, 46);
        symbol->setAlignment(Qt::AlignCenter);
        symbol->setPixmap(makeIcon(icon, QColor("#c9b0ff")).pixmap(QSize(27, 27)));
        column->addWidget(symbol);
        column->addSpacing(2);
        const QString title = fullScreen ? QStringLiteral("Весь экран") : QStringLiteral("Область экрана");
        column->addWidget(makeLabel(title, "cardHeading", card));
        auto *description = makeLabel(fullScreen ? QStringLiteral("Снимок текущего монитора")
                                                  : QStringLiteral("Выбор на застывшем экране"), "muted", card);
        description->setWordWrap(true);
        column->addWidget(description);
        column->addSpacing(10);
        auto *controls = new QHBoxLayout;
        auto *button = new QPushButton(fullScreen ? QStringLiteral("Снять") : QStringLiteral("Выбрать"), card);
        button->setAccessibleName(fullScreen ? QStringLiteral("Снять весь экран") : QStringLiteral("Выбрать область экрана"));
        if (!fullScreen) {
            button->setObjectName(QStringLiteral("primary"));
        }
        QObject::connect(button, &QPushButton::clicked, &window,
                         fullScreen ? &EditorWindow::fullCaptureRequested : &EditorWindow::regionCaptureRequested);
        key = makeLabel({}, "key", card);
        key->hide();
        controls->addWidget(button);
        controls->addStretch();
        controls->addWidget(key);
        column->addLayout(controls);
        return card;
    }

    QWidget *makeEditor()
    {
        auto *page = new QWidget(pages);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *body = new QHBoxLayout;
        body->setSpacing(0);
        body->addWidget(makeRail(page));

        workspace = new Workspace(page);
        workspace->setFrameShape(QFrame::NoFrame);
        workspace->setAlignment(Qt::AlignCenter);
        canvas = new ScreenshotCanvas;
        workspace->setWidget(canvas);
        workspace->onResize = [this] { if (fit) updateZoom(); };
        workspace->onZoom = [this](int direction) { stepZoom(direction); };
        canvas->onCommit = [this](Annotation annotation) {
            if (cleanCount > historyCount) {
                cleanCount = -1;
            }
            annotations.resize(historyCount);
            annotations.push_back(std::move(annotation));
            applyNextAnnotation();
        };
        body->addWidget(workspace, 1);
        layout->addLayout(body, 1);
        layout->addWidget(makeFooter(page));
        return page;
    }

    QWidget *makeRail(QWidget *parent)
    {
        auto *rail = new QWidget(parent);
        rail->setObjectName(QStringLiteral("rail"));
        rail->setFixedWidth(162);
        auto *column = new QVBoxLayout(rail);
        column->setContentsMargins(11, 12, 11, 10);
        column->setSpacing(4);
        column->addWidget(makeLabel(QStringLiteral("Инструменты"), "section", rail));
        column->addSpacing(3);
        tools = new QButtonGroup(rail);
        struct ToolChoice { Tool tool; Icon icon; const char *label; const char *key; };
        const std::array choices{
            ToolChoice{Tool::Arrow, Icon::Arrow, "Стрелка", "A"},
            ToolChoice{Tool::Pen, Icon::Pen, "Перо", "P"},
            ToolChoice{Tool::Rectangle, Icon::Rectangle, "Рамка", "R"},
            ToolChoice{Tool::Blur, Icon::Blur, "Размытие", "B"},
            ToolChoice{Tool::Redact, Icon::Redact, "Скрыть", "X"},
        };
        for (const auto &choice : choices) {
            const QString label = QString::fromUtf8(choice.label);
            const QString key = QString::fromLatin1(choice.key);
            auto *button = makeButton(label, choice.icon, rail, "tool");
            button->setCheckable(true);
            button->setToolTip(label + QStringLiteral(" · ") + key);
            tools->addButton(button, static_cast<int>(choice.tool));
            column->addWidget(button);
            addShortcut(QKeySequence(key), [this, button] { if (!source.isNull()) button->click(); });
        }
        tools->button(static_cast<int>(Tool::Arrow))->setChecked(true);
        QObject::connect(tools, &QButtonGroup::idClicked, &window, [this](int id) {
            canvas->cancelStroke();
            canvas->tool = static_cast<Tool>(id);
            status->clear();
            refreshControls();
        });

        auto *divider = new QFrame(rail);
        divider->setObjectName(QStringLiteral("divider"));
        divider->setFrameShape(QFrame::HLine);
        column->addSpacing(5);
        column->addWidget(divider);
        column->addSpacing(5);
        column->addWidget(makeLabel(QStringLiteral("Цвет"), "section", rail));
        color = new QComboBox(rail);
        color->setAccessibleName(QStringLiteral("Цвет линии"));
        const std::array colors{
            std::pair{"Красный", "#ff5373"}, std::pair{"Оранжевый", "#ff9e4b"},
            std::pair{"Жёлтый", "#ffdb6a"}, std::pair{"Зелёный", "#60d9a2"},
            std::pair{"Голубой", "#6db6ff"}, std::pair{"Фиолетовый", "#b58aff"},
            std::pair{"Белый", "#ffffff"}, std::pair{"Чёрный", "#111420"},
        };
        for (const auto &[name, value] : colors) {
            const QColor swatch(QString::fromLatin1(value));
            QPixmap pixmap(14, 14);
            pixmap.fill(swatch);
            color->addItem(QIcon(pixmap), QString::fromUtf8(name), swatch);
        }
        QObject::connect(color, &QComboBox::currentIndexChanged, &window, [this] {
            canvas->color = color->currentData().value<QColor>();
        });
        column->addWidget(color);

        column->addSpacing(3);
        column->addWidget(makeLabel(QStringLiteral("Толщина"), "section", rail));
        thickness = new QComboBox(rail);
        thickness->setAccessibleName(QStringLiteral("Толщина линии"));
        for (const int pixels : {2, 4, 6, 10, 16, 24}) {
            thickness->addItem(QStringLiteral("%1 пикс.").arg(pixels), pixels);
        }
        thickness->setCurrentIndex(2);
        QObject::connect(thickness, &QComboBox::currentIndexChanged, &window, [this] {
            canvas->strokeWidth = thickness->currentData().toInt();
        });
        column->addWidget(thickness);
        column->addStretch();
        return rail;
    }

    QWidget *makeFooter(QWidget *parent)
    {
        auto *footer = new QWidget(parent);
        footer->setObjectName(QStringLiteral("footer"));
        auto *row = new QHBoxLayout(footer);
        row->setContentsMargins(12, 6, 12, 6);
        row->setSpacing(6);
        undo = makeIconButton(Icon::Undo, QStringLiteral("Отменить"), footer);
        redo = makeIconButton(Icon::Redo, QStringLiteral("Повторить"), footer);
        QObject::connect(undo, &QToolButton::clicked, &window, [this] { moveHistory(-1); });
        QObject::connect(redo, &QToolButton::clicked, &window, [this] { moveHistory(1); });
        row->addWidget(undo);
        row->addWidget(redo);
        row->addSpacing(7);
        dimensions = makeLabel({}, "muted", footer);
        status = makeLabel({}, "status", footer);
        row->addWidget(dimensions);
        row->addSpacing(9);
        row->addWidget(status, 1);

        auto *fitButton = new QPushButton(QStringLiteral("Вписать"), footer);
        fitButton->setToolTip(QStringLiteral("Вписать снимок в окно · 0"));
        QObject::connect(fitButton, &QPushButton::clicked, &window, [this] { setFit(); });
        row->addWidget(fitButton);
        auto *minus = makeIconButton(Icon::Minus, QStringLiteral("Уменьшить масштаб"), footer);
        auto *plus = makeIconButton(Icon::Plus, QStringLiteral("Увеличить масштаб"), footer);
        auto *actual = new QPushButton(QStringLiteral("1:1"), footer);
        actual->setToolTip(QStringLiteral("Исходный размер · 1"));
        QObject::connect(minus, &QToolButton::clicked, &window, [this] { stepZoom(-1); });
        QObject::connect(plus, &QToolButton::clicked, &window, [this] { stepZoom(1); });
        QObject::connect(actual, &QPushButton::clicked, &window, [this] { setZoom(1); });
        zoomLabel = makeLabel({}, "muted", footer);
        zoomLabel->setAlignment(Qt::AlignCenter);
        zoomLabel->setMinimumWidth(48);
        row->addWidget(minus);
        row->addWidget(zoomLabel);
        row->addWidget(plus);
        row->addWidget(actual);
        return footer;
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
        addShortcut(QKeySequence(Qt::Key_Minus), [this] { stepZoom(-1); });
    }

    void addShortcut(const QKeySequence &sequence, const std::function<void()> &handler)
    {
        auto *shortcut = new QShortcut(sequence, &window);
        QObject::connect(shortcut, &QShortcut::activated, &window, handler);
    }

    void refreshControls()
    {
        const bool hasImage = !source.isNull();
        full->setVisible(hasImage);
        region->setVisible(hasImage);
        save->setVisible(hasImage);
        copy->setVisible(hasImage);
        undo->setEnabled(historyCount > 0);
        redo->setEnabled(historyCount < annotations.size());
        const bool drawsLine = canvas->tool != Tool::Blur && canvas->tool != Tool::Redact;
        color->setEnabled(drawsLine);
        thickness->setEnabled(drawsLine);
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
        dimensions->setText(QStringLiteral("%1 × %2 пикс.").arg(source.width()).arg(source.height()));
        updateImage();
        updateZoom();
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
                refreshImageControls();
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
        screenframe::applyAnnotation(canvas->image, annotation);
        ++historyCount;
        canvas->updatePixels(area);
        refreshImageControls();
    }

    void updateImage()
    {
        canvas->image = screenframe::renderAnnotations(source, annotations, historyCount);
        canvas->update();
        refreshImageControls();
    }

    void refreshImageControls()
    {
        status->clear();
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
        zoom = std::clamp(value, 0.025, 4.0);
        updateZoom();
    }

    void updateZoom()
    {
        if (source.isNull()) {
            return;
        }
        const qreal dpr = workspace->devicePixelRatioF();
        if (fit) {
            const QSize available = workspace->maximumViewportSize() - QSize(42, 42);
            zoom = std::min({1.0, qreal(std::max(1, available.width())) * dpr / source.width(),
                            qreal(std::max(1, available.height())) * dpr / source.height()});
        }
        const QSize size(std::max(1, qRound(source.width() * zoom / dpr)),
                         std::max(1, qRound(source.height() * zoom / dpr)));
        const QPoint viewCenter(workspace->viewport()->width() / 2, workspace->viewport()->height() / 2);
        const QPoint oldCenter = canvas->mapFrom(workspace->viewport(), viewCenter);
        const QPointF relative(qreal(oldCenter.x()) / std::max(1, canvas->width()),
                               qreal(oldCenter.y()) / std::max(1, canvas->height()));
        canvas->setFixedSize(size);
        if (!fit) {
            workspace->horizontalScrollBar()->setValue(qRound(relative.x() * size.width()) - viewCenter.x());
            workspace->verticalScrollBar()->setValue(qRound(relative.y() * size.height()) - viewCenter.y());
        }
        zoomLabel->setText(QStringLiteral("%1 %").arg(qRound(zoom * 100)));
    }

    void copyImage()
    {
        if (canvas->image.isNull()) {
            return;
        }
        QApplication::clipboard()->setImage(canvas->image);
        cleanCount = historyCount;
        refreshControls();
        status->setText(QStringLiteral("Снимок скопирован"));
    }

    void saveImage()
    {
        if (canvas->image.isNull()) {
            return;
        }
        QDir directory(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
        if (!directory.exists()) {
            directory = QDir::home();
        }
        const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
        QFileDialog dialog(&window, QStringLiteral("Сохранить снимок"), directory.absolutePath(), QStringLiteral("PNG (*.png)"));
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix(QStringLiteral("png"));
        dialog.selectFile(QStringLiteral("Снимок_%1.png").arg(timestamp));
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
            return;
        }
        QString error;
        if (!savePng(canvas->image, dialog.selectedFiles().constFirst(), &error)) {
            QMessageBox::warning(&window, QStringLiteral("Не удалось сохранить снимок"), error);
            return;
        }
        cleanCount = historyCount;
        refreshControls();
        status->setText(QStringLiteral("Снимок сохранён"));
    }
};

EditorWindow::EditorWindow(QWidget *parent) : QMainWindow(parent), ui(std::make_unique<Ui>(*this)) {}

EditorWindow::~EditorWindow() = default;

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

void EditorWindow::setShortcuts(const QString &full, const QString &region, const QString &warning)
{
    ui->fullKey->setText(full);
    ui->regionKey->setText(region);
    ui->fullKey->setVisible(!full.isEmpty());
    ui->regionKey->setVisible(!region.isEmpty());
    ui->full->setToolTip(full);
    ui->region->setToolTip(region);
    ui->warning->setText(warning);
    ui->warning->setVisible(!warning.isEmpty());
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
    QMessageBox dialog(QMessageBox::Question, QStringLiteral("Новый снимок"),
                       QStringLiteral("На текущем снимке есть несохранённые правки."), QMessageBox::NoButton, this);
    auto *replace = dialog.addButton(QStringLiteral("Продолжить"), QMessageBox::AcceptRole);
    auto *cancel = dialog.addButton(QStringLiteral("Отмена"), QMessageBox::RejectRole);
    dialog.setDefaultButton(cancel);
    dialog.exec();
    return dialog.clickedButton() == replace;
}
