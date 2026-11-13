#include "librarywindow.h"

#include "controls.h"
#include "libraryclient.h"
#include "pngexport.h"
#include "theme.h"

#include <QAbstractListModel>
#include <QApplication>
#include <QCache>
#include <QClipboard>
#include <QHBoxLayout>
#include <QImage>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

constexpr int minimumTileWidth = 216;
constexpr int gutter = 8;
constexpr int metaHeight = 48;
constexpr QSize thumbnailBounds(480, 300);

QString itemTitle(const LibraryItem &item)
{
    if (item.kind == QStringLiteral("full")) {
        return QStringLiteral("Full screen");
    }
    if (item.kind == QStringLiteral("region")) {
        return QStringLiteral("Region");
    }
    return QStringLiteral("Screenshot");
}

QString itemTime(const LibraryItem &item)
{
    const QDateTime local = item.capturedAt.toLocalTime();
    const QDate today = QDate::currentDate();
    const QString time = local.toString(QStringLiteral("HH:mm"));
    if (local.date() == today) {
        return QStringLiteral("Today, %1").arg(time);
    }
    if (local.date() == today.addDays(-1)) {
        return QStringLiteral("Yesterday, %1").arg(time);
    }
    if (local.date().year() == today.year()) {
        return QLocale().toString(local, QStringLiteral("d MMM, HH:mm"));
    }
    return QLocale().toString(local, QStringLiteral("d MMM yyyy"));
}

QString itemSize(const LibraryItem &item)
{
    return QStringLiteral("%1 × %2").arg(item.size.width()).arg(item.size.height());
}

QString itemDetails(const LibraryItem &item)
{
    QStringList parts{itemTime(item)};
    if (!item.device.isEmpty()) {
        parts << item.device;
    }
    parts << itemSize(item) + QStringLiteral(" px");
    return parts.join(QStringLiteral("  ·  "));
}

class LibraryModel final : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : int(items.size());
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= items.size()) {
            return {};
        }
        const auto &item = items.at(index.row());
        if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) {
            return itemTitle(item);
        }
        if (role == Qt::ToolTipRole || role == Qt::AccessibleDescriptionRole) {
            return itemDetails(item);
        }
        return {};
    }

    const LibraryItem &at(int row) const { return items.at(row); }

    void replace(QList<LibraryItem> entries)
    {
        beginResetModel();
        items = std::move(entries);
        endResetModel();
    }

    void append(QList<LibraryItem> entries)
    {
        if (!entries.isEmpty()) {
            beginInsertRows({}, rowCount(), rowCount() + int(entries.size()) - 1);
            items.append(std::move(entries));
            endInsertRows();
        }
    }

    const QImage *thumbnail(const QString &id) const { return thumbnails.object(id); }
    bool failed(const QString &id) const { return failures.contains(id); }

    void setThumbnail(int row, QImage image)
    {
        if (image.isNull()) {
            return;
        }
        image.setDevicePixelRatio(1);
        if (image.width() > thumbnailBounds.width() || image.height() > thumbnailBounds.height()) {
            image = image.scaled(thumbnailBounds, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        thumbnails.insert(at(row).id, new QImage(std::move(image)));
        const QModelIndex updated = index(row);
        emit dataChanged(updated, updated, {Qt::DecorationRole});
    }

    void setFailed(int row)
    {
        failures.insert(at(row).id);
        const QModelIndex updated = index(row);
        emit dataChanged(updated, updated, {Qt::DecorationRole});
    }

    void clearFailures() { failures.clear(); }

private:
    QList<LibraryItem> items;
    QSet<QString> failures;
    QCache<QString, QImage> thumbnails{120};
};

class LibraryDelegate final : public QStyledItemDelegate {
public:
    LibraryDelegate(QListView *view) : QStyledItemDelegate(view), view(view) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return view->gridSize();
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const theme::Palette &c = theme::colors();
        const auto *model = static_cast<const LibraryModel *>(index.model());
        const auto &item = model->at(index.row());
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);

        const QRectF card = QRectF(option.rect).adjusted(gutter, gutter, -gutter, -gutter / 2.0);
        const QRectF frame(card.left(), card.top(), card.width(), card.height() - metaHeight);
        QPainterPath shape;
        shape.addRoundedRect(frame, 10, 10);
        painter->fillPath(shape, c.dark ? c.raised : c.controlHover);
        painter->save();
        painter->setClipPath(shape);
        if (const QImage *thumbnail = model->thumbnail(item.id)) {
            QSizeF fitted = thumbnail->size();
            fitted.scale(frame.size().shrunkBy(QMarginsF(10, 10, 10, 10)), Qt::KeepAspectRatio);
            QRectF destination(QPointF(), fitted);
            destination.moveCenter(frame.center());
            painter->setPen(Qt::NoPen);
            theme::paintShadow(*painter, destination, 2, 6, 2, c.dark ? 1.2 : 0.7);
            painter->drawImage(destination, *thumbnail);
        } else {
            const QRectF glyph(frame.center().x() - 13, frame.center().y() - 13, 26, 26);
            paintIcon(*painter, model->failed(item.id) || !item.hasThumbnail ? Icon::Library : Icon::Region, glyph,
                      c.textFaint, 1.6);
        }
        if (hovered && !selected) {
            painter->fillRect(frame, theme::withAlpha(c.text, 0.04));
        }
        painter->restore();
        if (selected) {
            painter->setPen(QPen(c.accent, 2));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(frame.adjusted(-2, -2, 2, 2), 12, 12);
        } else {
            painter->setPen(QPen(hovered ? c.borderStrong : theme::withAlpha(c.border, 0.8), 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(frame.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
        }

        const qreal textTop = frame.bottom() + 9;
        const int textWidth = int(card.width()) - 2;
        const QFont titleFont = theme::font(13, theme::Weight::Semibold);
        const QFont metaFont = theme::font(12);
        const QString title = itemTitle(item);
        const int titleWidth = QFontMetrics(titleFont).horizontalAdvance(title);
        painter->setFont(titleFont);
        painter->setPen(c.text);
        painter->drawText(QRectF(card.left() + 1, textTop, textWidth, 18), Qt::AlignLeft | Qt::AlignVCenter, title);
        painter->setFont(metaFont);
        painter->setPen(c.textMuted);
        const int sizeLeft = titleWidth + 8;
        painter->drawText(QRectF(card.left() + 1 + sizeLeft, textTop, textWidth - sizeLeft, 18),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(metaFont).elidedText(itemSize(item), Qt::ElideRight, textWidth - sizeLeft));
        QString meta = itemTime(item);
        if (!item.device.isEmpty()) {
            meta += QStringLiteral("  ·  ") + item.device;
        }
        painter->drawText(QRectF(card.left() + 1, textTop + 19, textWidth, 17), Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(metaFont).elidedText(meta, Qt::ElideRight, textWidth));
        painter->restore();
    }

private:
    QListView *view;
};

class LibraryGrid final : public QListView {
public:
    explicit LibraryGrid(QWidget *parent) : QListView(parent)
    {
        setViewMode(QListView::IconMode);
        setMovement(QListView::Static);
        setResizeMode(QListView::Adjust);
        setFlow(QListView::LeftToRight);
        setWrapping(true);
        setUniformItemSizes(true);
        setSpacing(0);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setFrameShape(QFrame::NoFrame);
        setMouseTracking(true);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setViewportMargins(10, 8, 10, 12);
        verticalScrollBar()->setSingleStep(24);
        setGridSize(QSize(minimumTileWidth, minimumTileWidth));
    }

    std::function<void()> resized;

    int columns() const { return std::max(1, viewport()->width() / std::max(1, gridSize().width())); }

protected:
    bool viewportEvent(QEvent *event) override
    {
        // The viewport also narrows when the scroll bar appears, without resizing the view.
        if (event->type() == QEvent::Resize) {
            fitGrid();
        }
        return QListView::viewportEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QListView::resizeEvent(event);
        fitGrid();
    }

private:
    void fitGrid()
    {
        // Icon mode reserves room for a vertical scroll bar again when it lays out a wrapping grid.
        const int available = viewport()->width() - verticalScrollBar()->sizeHint().width() - 1;
        const int columns = std::max(1, available / minimumTileWidth);
        const int tile = available / columns;
        const int imageHeight = (tile - 2 * gutter) * 10 / 16;
        const QSize grid(tile, imageHeight + metaHeight + gutter + gutter / 2);
        if (grid != gridSize()) {
            setGridSize(grid);
        }
        if (resized) {
            resized();
        }
    }
};

// A single line that ends in an ellipsis when it does not fit.
class ElidedLabel final : public QLabel {
public:
    ElidedLabel(const char *role, QWidget *parent) : QLabel(parent)
    {
        setObjectName(QString::fromLatin1(role));
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

    QSize minimumSizeHint() const override { return QSize(0, QLabel::minimumSizeHint().height()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        painter.setFont(font());
        const QRect area = contentsRect();
        painter.drawText(area, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideRight, area.width()));
    }
};

class ImageCanvas final : public QWidget {
public:
    using QWidget::QWidget;
    QImage image;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(rect(), image);
    }
};

class ImageView final : public QScrollArea {
public:
    explicit ImageView(QWidget *parent) : QScrollArea(parent), holder(new QWidget), canvas(new ImageCanvas(holder))
    {
        setFrameShape(QFrame::NoFrame);
        setAlignment(Qt::AlignCenter);
        setWidget(holder);
        holder->setAutoFillBackground(false);
        canvas->setAccessibleName(QStringLiteral("Screenshot preview"));
    }

    void setImage(QImage image)
    {
        canvas->image = std::move(image);
        fit = true;
        resizeCanvas();
        canvas->update();
    }

    void setFit(bool value)
    {
        fit = value;
        resizeCanvas();
    }

    const QImage &image() const { return canvas->image; }

protected:
    bool viewportEvent(QEvent *event) override
    {
        if (event->type() != QEvent::Paint) {
            return QScrollArea::viewportEvent(event);
        }
        const theme::Palette &c = theme::colors();
        QPainter painter(viewport());
        painter.fillRect(static_cast<QPaintEvent *>(event)->rect(), c.stage);
        if (!canvas->image.isNull()) {
            const QRect frame(canvas->mapTo(viewport(), QPoint(0, 0)), canvas->size());
            theme::paintShadow(painter, QRectF(frame), 3, 16, 5, c.dark ? 1.6 : 1.1);
        }
        return true;
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QScrollArea::resizeEvent(event);
        resizeCanvas();
    }

private:
    static constexpr int padding = 32;
    QWidget *holder;
    ImageCanvas *canvas;
    bool fit = true;

    void resizeCanvas()
    {
        const QSize imageSize = image().size();
        if (image().isNull()) {
            holder->setFixedSize(1, 1);
            canvas->setGeometry(0, 0, 1, 1);
            return;
        }
        const qreal dpr = devicePixelRatioF();
        QSize target(qMax(1, qRound(imageSize.width() / dpr)), qMax(1, qRound(imageSize.height() / dpr)));
        if (fit) {
            const QSize available = (maximumViewportSize() - QSize(2 * padding, 2 * padding)).expandedTo(QSize(1, 1));
            target.scale(available.boundedTo(target), Qt::KeepAspectRatio);
        }
        target = target.expandedTo(QSize(1, 1));
        holder->setFixedSize(target + QSize(2 * padding, 2 * padding));
        canvas->setGeometry(padding, padding, target.width(), target.height());
        viewport()->update();
    }
};

class Bar final : public QWidget {
public:
    explicit Bar(QWidget *parent) : QWidget(parent) { setFixedHeight(56); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const theme::Palette &c = theme::colors();
        QPainter painter(this);
        painter.fillRect(rect(), c.surface);
        painter.fillRect(QRectF(0, height() - 1, width(), 1), c.border);
    }
};

class Overlay final : public QWidget {
public:
    using QWidget::QWidget;
    std::function<void()> onResize;

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        if (onResize) {
            onResize();
        }
    }
};

constexpr auto libraryRules = R"(
    QListView { background: @stage; border: none; outline: none; }
    QLabel#barTitle { font-size: 16px; font-weight: 600; }
    QLabel#viewerTitle { font-size: 14px; font-weight: 600; }
)";

enum class StateAction { None, Setup, Retry };

} // namespace

struct LibraryWindow::Ui {
    Ui(LibraryWindow &owner, LibraryClient &transport) : window(owner), client(transport)
    {
        window.setWindowTitle(QStringLiteral("Library — ScreenFrame"));
        window.setMinimumSize(560, 420);
        window.resize(980, 700);

        auto *root = new QWidget(&window);
        root->setObjectName(QStringLiteral("surface"));
        root->setAttribute(Qt::WA_StyledBackground);
        theme::attach(root, libraryRules);
        window.setCentralWidget(root);
        auto *layout = new QVBoxLayout(root);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        pages = new QStackedWidget(root);
        layout->addWidget(pages);
        listPage = makeListPage();
        viewerPage = makeViewerPage();
        pages->addWidget(listPage);
        pages->addWidget(viewerPage);

        thumbnailTimer = new QTimer(&window);
        thumbnailTimer->setSingleShot(true);
        thumbnailTimer->setInterval(25);
        QObject::connect(thumbnailTimer, &QTimer::timeout, &context, [this] { loadThumbnails(); });
        grid->resized = [this] { scheduleThumbnails(); };
        QObject::connect(grid->verticalScrollBar(), &QScrollBar::valueChanged, &context, [this] {
            scheduleThumbnails();
            loadMoreIfNeeded();
        });

        refreshTimer = new QTimer(&window);
        refreshTimer->setSingleShot(true);
        refreshTimer->setInterval(250);
        QObject::connect(refreshTimer, &QTimer::timeout, &context, [this] {
            if (window.isVisible() && pages->currentWidget() == listPage && needsRefresh && !listing) {
                refresh();
            }
        });
        QObject::connect(&client, &LibraryClient::libraryChanged, &context, [this] {
            needsRefresh = true;
            if (window.isVisible() && pages->currentWidget() == listPage) {
                refreshTimer->start();
            }
        });
        QObject::connect(&client, &LibraryClient::syncStatusChanged, &context,
                         [this](int pending, const QString &error) { updateSync(pending, error); });
        updateSync(client.pendingCount(), {});

        addShortcut(QKeySequence(Qt::Key_Escape), [this] {
            if (pages->currentWidget() == viewerPage) {
                showList();
            } else {
                window.close();
            }
        });
        addShortcut(QKeySequence::Refresh, [this] {
            if (pages->currentWidget() == listPage && client.isConfigured()) {
                refresh();
            }
        });
        addShortcut(QKeySequence::Save, [this] { saveImage(); });
        addShortcut(QKeySequence::Copy, [this] { copyImage(); });
        // Arrow keys belong to the grid while it is showing, so these only apply inside the viewer.
        addShortcut(QKeySequence(Qt::Key_Left), [this] { step(-1); }, viewerPage);
        addShortcut(QKeySequence(Qt::Key_Right), [this] { step(1); }, viewerPage);
        addShortcut(QKeySequence(Qt::Key_0), [this] { setViewerFit(true); }, viewerPage);
        addShortcut(QKeySequence(Qt::Key_1), [this] { setViewerFit(false); }, viewerPage);
    }

    ~Ui() { grid->resized = {}; }

    LibraryWindow &window;
    LibraryClient &client;
    QStackedWidget *pages = nullptr;
    QWidget *listPage = nullptr;
    QWidget *viewerPage = nullptr;
    QStackedWidget *listContent = nullptr;
    EmptyState *listState = nullptr;
    StateAction stateAction = StateAction::None;
    LibraryGrid *grid = nullptr;
    LibraryModel *model = nullptr;
    QLabel *listCaption = nullptr;
    Banner *listBanner = nullptr;
    Banner *syncBanner = nullptr;
    Button *refreshButton = nullptr;
    QTimer *thumbnailTimer = nullptr;
    QTimer *refreshTimer = nullptr;
    QSet<QString> pendingThumbnails;
    quint64 listGeneration = 0;
    bool needsRefresh = true;
    bool listing = false;
    QString nextCursor;
    int pendingUploads = 0;

    QStackedWidget *viewerContent = nullptr;
    EmptyState *viewerState = nullptr;
    ImageView *imageView = nullptr;
    Notice *viewerToast = nullptr;
    QLabel *viewerTitle = nullptr;
    QLabel *viewerDetails = nullptr;
    Button *previousButton = nullptr;
    Button *nextButton = nullptr;
    Segmented *zoomMode = nullptr;
    Button *copyButton = nullptr;
    Button *saveButton = nullptr;
    LibraryItem openedItem;
    int openedRow = -1;
    quint64 viewerGeneration = 0;
    QObject context;

    void addShortcut(const QKeySequence &sequence, const std::function<void()> &handler, QWidget *scope = nullptr)
    {
        auto *shortcut = new QShortcut(sequence, scope ? scope : static_cast<QWidget *>(&window));
        if (scope) {
            shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        }
        QObject::connect(shortcut, &QShortcut::activated, &context, handler);
    }

    QWidget *makeListPage()
    {
        auto *page = new QWidget(pages);
        auto *column = new QVBoxLayout(page);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        auto *bar = new Bar(page);
        auto *header = new QHBoxLayout(bar);
        header->setContentsMargins(20, 0, 12, 0);
        header->setSpacing(4);
        header->addWidget(theme::label(QStringLiteral("Library"), "barTitle", bar));
        header->addSpacing(12);
        listCaption = theme::label({}, "caption", bar);
        header->addWidget(listCaption, 1);
        refreshButton = new Button(Button::Ghost, Icon::Refresh, {}, bar);
        refreshButton->setToolTip(QStringLiteral("Refresh  ·  ") + QKeySequence(QKeySequence::Refresh).toString(QKeySequence::NativeText));
        refreshButton->setAccessibleName(QStringLiteral("Refresh"));
        QObject::connect(refreshButton, &QPushButton::clicked, &context, [this] { refresh(); });
        header->addWidget(refreshButton);
        auto *settingsButton = new Button(Button::Ghost, Icon::Settings, {}, bar);
        settingsButton->setToolTip(QStringLiteral("Library settings"));
        settingsButton->setAccessibleName(QStringLiteral("Settings"));
        QObject::connect(settingsButton, &QPushButton::clicked, &window, &LibraryWindow::settingsRequested);
        header->addWidget(settingsButton);
        column->addWidget(bar);

        syncBanner = new Banner(page);
        listBanner = new Banner(page);
        column->addWidget(syncBanner);
        column->addWidget(listBanner);

        listContent = new QStackedWidget(page);
        listState = new EmptyState(listContent);
        listState->setObjectName(QStringLiteral("stage"));
        listState->setAttribute(Qt::WA_StyledBackground);
        QObject::connect(listState->action(), &QPushButton::clicked, &context, [this] {
            if (stateAction == StateAction::Setup) {
                emit window.settingsRequested();
            } else if (stateAction == StateAction::Retry) {
                refresh();
            }
        });
        listContent->addWidget(listState);

        grid = new LibraryGrid(listContent);
        grid->setAccessibleName(QStringLiteral("Screenshots"));
        model = new LibraryModel(grid);
        grid->setModel(model);
        grid->setItemDelegate(new LibraryDelegate(grid));
        QObject::connect(grid, &QListView::activated, &context, [this](const QModelIndex &index) { openItem(index); });
        listContent->addWidget(grid);
        column->addWidget(listContent, 1);
        return page;
    }

    QWidget *makeViewerPage()
    {
        auto *page = new QWidget(pages);
        page->setFocusPolicy(Qt::StrongFocus);
        auto *column = new QVBoxLayout(page);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        auto *bar = new Bar(page);
        auto *row = new QHBoxLayout(bar);
        row->setContentsMargins(10, 0, 12, 0);
        row->setSpacing(4);
        auto *back = new Button(Button::Ghost, Icon::Back, QStringLiteral("Library"), bar);
        back->setToolTip(QStringLiteral("Back to all screenshots  ·  Esc"));
        QObject::connect(back, &QPushButton::clicked, &context, [this] { showList(); });
        row->addWidget(back);
        row->addWidget(separator(bar));
        row->addSpacing(6);
        auto *heading = new QVBoxLayout;
        heading->setSpacing(1);
        heading->setContentsMargins(0, 0, 0, 0);
        viewerTitle = new ElidedLabel("viewerTitle", bar);
        viewerDetails = new ElidedLabel("caption", bar);
        heading->addStretch();
        heading->addWidget(viewerTitle);
        heading->addWidget(viewerDetails);
        heading->addStretch();
        row->addLayout(heading, 1);

        previousButton = new Button(Button::Ghost, Icon::Back, {}, bar);
        nextButton = new Button(Button::Ghost, Icon::Forward, {}, bar);
        previousButton->setToolTip(QStringLiteral("Previous screenshot  ·  ←"));
        nextButton->setToolTip(QStringLiteral("Next screenshot  ·  →"));
        previousButton->setAccessibleName(QStringLiteral("Previous screenshot"));
        nextButton->setAccessibleName(QStringLiteral("Next screenshot"));
        QObject::connect(previousButton, &QPushButton::clicked, &context, [this] { step(-1); });
        QObject::connect(nextButton, &QPushButton::clicked, &context, [this] { step(1); });
        row->addWidget(previousButton);
        row->addWidget(nextButton);
        row->addSpacing(8);
        zoomMode = new Segmented({QStringLiteral("Fit"), QStringLiteral("100%")}, bar);
        zoomMode->setFixedWidth(124);
        zoomMode->setAccessibleName(QStringLiteral("Zoom"));
        zoomMode->setToolTip(QStringLiteral("Fit  ·  0     Actual size  ·  1"));
        QObject::connect(zoomMode, &Segmented::currentChanged, &context, [this](int index) { imageView->setFit(index == 0); });
        row->addWidget(zoomMode);
        row->addSpacing(8);
        copyButton = new Button(Button::Tonal, Icon::Copy, QStringLiteral("Copy"), bar);
        saveButton = new Button(Button::Primary, Icon::Save, QStringLiteral("Save"), bar);
        copyButton->setToolTip(QStringLiteral("Copy to clipboard"));
        saveButton->setToolTip(QStringLiteral("Save as PNG"));
        QObject::connect(copyButton, &QPushButton::clicked, &context, [this] { copyImage(); });
        QObject::connect(saveButton, &QPushButton::clicked, &context, [this] { saveImage(); });
        row->addWidget(copyButton);
        row->addSpacing(4);
        row->addWidget(saveButton);
        column->addWidget(bar);

        auto *area = new Overlay(page);
        viewerContent = new QStackedWidget(area);
        viewerState = new EmptyState(viewerContent);
        viewerState->setObjectName(QStringLiteral("stage"));
        viewerState->setAttribute(Qt::WA_StyledBackground);
        QObject::connect(viewerState->action(), &QPushButton::clicked, &context, [this] { requestImage(); });
        viewerContent->addWidget(viewerState);
        imageView = new ImageView(viewerContent);
        viewerContent->addWidget(imageView);
        viewerToast = new Notice(Notice::Success, area);
        const auto place = [this, area] {
            viewerContent->setGeometry(area->rect());
            if (viewerToast->isVisible()) {
                const QSize size = viewerToast->sizeFor(std::min(area->width() - 32, 480));
                viewerToast->setGeometry((area->width() - size.width()) / 2, 14 - Notice::shadowMargin, size.width(),
                                         size.height());
                viewerToast->raise();
            }
        };
        area->onResize = place;
        QObject::connect(viewerToast, &Notice::messageChanged, &context, place);
        column->addWidget(area, 1);
        return page;
    }

    void show()
    {
        if (needsRefresh && !listing && pages->currentWidget() == listPage) {
            refresh();
        }
        scheduleThumbnails();
    }

    void setListState(Icon icon, const QString &title, const QString &details, StateAction action = StateAction::None)
    {
        stateAction = action;
        const QString label = action == StateAction::Setup ? QStringLiteral("Set up library")
            : action == StateAction::Retry                 ? QStringLiteral("Try again")
                                                           : QString();
        listState->set(icon, title, details, label);
        listContent->setCurrentWidget(listState);
    }

    void refresh()
    {
        model->clearFailures();
        if (!client.isConfigured()) {
            needsRefresh = true;
            refreshButton->setEnabled(false);
            listCaption->clear();
            setListState(Icon::Library, QStringLiteral("Collect screenshots from all your devices"),
                         QStringLiteral("Connect ScreenFrame to a library you host. Screenshots from every connected "
                                        "device appear here, ready to save again."),
                         StateAction::Setup);
            return;
        }
        requestList({});
    }

    void requestList(const QString &cursor)
    {
        if (listing) {
            return;
        }
        const bool first = cursor.isEmpty();
        if (first) {
            needsRefresh = false;
        }
        listing = true;
        refreshButton->setEnabled(false);
        listBanner->setMessage({});
        if (first && model->rowCount() == 0) {
            setListState(Icon::Refresh, QStringLiteral("Loading screenshots…"), {});
            listCaption->clear();
        }
        client.list(cursor, &context, [this, first](QList<LibraryItem> entries, QString next, QString error) {
            listing = false;
            refreshButton->setEnabled(true);
            if (!error.isEmpty()) {
                if (model->rowCount() == 0) {
                    setListState(Icon::Warning, QStringLiteral("Couldn’t reach the library"), error, StateAction::Retry);
                } else {
                    listBanner->setMessage(error);
                }
                return;
            }
            nextCursor = std::move(next);
            if (first) {
                const QModelIndex current = grid->currentIndex();
                const QString selectedId = current.isValid() ? model->at(current.row()).id : QString();
                ++listGeneration;
                model->replace(std::move(entries));
                grid->scrollToTop();
                if (!selectedId.isEmpty()) {
                    for (int row = 0; row < model->rowCount(); ++row) {
                        if (model->at(row).id == selectedId) {
                            grid->setCurrentIndex(model->index(row));
                            break;
                        }
                    }
                }
            } else {
                model->append(std::move(entries));
                if (openedRow >= 0 && pages->currentWidget() == viewerPage) {
                    nextButton->setEnabled(openedRow + 1 < model->rowCount());
                }
            }
            if (model->rowCount() == 0) {
                setListState(Icon::Region, QStringLiteral("No screenshots yet"),
                             QStringLiteral("Screenshots archived from any device connected to this library appear here."));
            } else {
                listContent->setCurrentWidget(grid);
                if (!grid->currentIndex().isValid()) {
                    grid->setCurrentIndex(model->index(0));
                }
            }
            updateCaption();
            if (needsRefresh) {
                refreshTimer->start();
            }
            scheduleThumbnails();
            QTimer::singleShot(0, &context, [this] { loadMoreIfNeeded(); });
        });
    }

    void loadMoreIfNeeded()
    {
        if (listing || nextCursor.isEmpty() || listContent->currentWidget() != grid || !window.isVisible()) {
            return;
        }
        const QScrollBar *bar = grid->verticalScrollBar();
        if (bar->value() >= bar->maximum() - grid->gridSize().height()) {
            requestList(nextCursor);
        }
    }

    void updateCaption()
    {
        QStringList parts;
        const int count = model->rowCount();
        if (count > 0) {
            parts << (nextCursor.isEmpty() ? QStringLiteral("%n screenshot(s)") : QStringLiteral("%n+ screenshots"))
                         .replace(QStringLiteral("%n"), QString::number(count))
                         .replace(QStringLiteral("(s)"), count == 1 ? QString() : QStringLiteral("s"));
        }
        if (pendingUploads > 0) {
            parts << QStringLiteral("%1 waiting to upload").arg(pendingUploads);
        }
        listCaption->setText(parts.join(QStringLiteral("  ·  ")));
    }

    void scheduleThumbnails()
    {
        if (thumbnailTimer && window.isVisible() && pages->currentWidget() == listPage && grid->isVisible()) {
            thumbnailTimer->start();
        }
    }

    void loadThumbnails()
    {
        if (!window.isVisible() || pages->currentWidget() != listPage || !grid->isVisible()) {
            return;
        }
        constexpr int maxRequests = 4;
        const QModelIndex firstVisible = grid->indexAt(QPoint(4, 4));
        if (!firstVisible.isValid()) {
            return;
        }
        const int columns = grid->columns();
        const int rows = grid->viewport()->height() / std::max(1, grid->gridSize().height()) + 2;
        const int first = std::max(0, firstVisible.row() - columns);
        const int last = std::min(model->rowCount(), firstVisible.row() + columns * (rows + 1));
        for (int row = first; row < last && pendingThumbnails.size() < maxRequests; ++row) {
            const LibraryItem &item = model->at(row);
            if (!item.hasThumbnail || model->thumbnail(item.id) || model->failed(item.id) || pendingThumbnails.contains(item.id)) {
                continue;
            }
            const QString id = item.id;
            pendingThumbnails.insert(id);
            const quint64 generation = listGeneration;
            client.image(id, true, &context, [this, id, row, generation](QImage image, QString error) {
                pendingThumbnails.remove(id);
                if (generation == listGeneration && row < model->rowCount() && model->at(row).id == id) {
                    if (error.isEmpty() && !image.isNull()) {
                        model->setThumbnail(row, std::move(image));
                    } else {
                        model->setFailed(row);
                    }
                }
                scheduleThumbnails();
            });
        }
    }

    void updateSync(int pending, const QString &error)
    {
        pendingUploads = pending;
        syncBanner->setMessage(error.isEmpty() ? QString() : QStringLiteral("Uploads paused: %1").arg(error));
        updateCaption();
    }

    void openItem(const QModelIndex &index)
    {
        if (!index.isValid() || index.row() >= model->rowCount()) {
            return;
        }
        openedRow = index.row();
        openedItem = model->at(openedRow);
        viewerTitle->setText(itemTitle(openedItem));
        viewerDetails->setText(itemDetails(openedItem));
        previousButton->setEnabled(openedRow > 0);
        nextButton->setEnabled(openedRow + 1 < model->rowCount());
        pages->setCurrentWidget(viewerPage);
        viewerPage->setFocus();
        requestImage();
    }

    void step(int direction)
    {
        if (pages->currentWidget() != viewerPage || openedRow < 0) {
            return;
        }
        const int row = openedRow + direction;
        if (row < 0 || row >= model->rowCount()) {
            return;
        }
        grid->setCurrentIndex(model->index(row));
        openItem(model->index(row));
        if (row + 2 >= model->rowCount()) {
            if (!listing && !nextCursor.isEmpty()) {
                requestList(nextCursor);
            }
        }
    }

    void setViewerFit(bool fit)
    {
        if (pages->currentWidget() == viewerPage && viewerContent->currentWidget() == imageView) {
            zoomMode->setCurrent(fit ? 0 : 1);
        }
    }

    void showList()
    {
        ++viewerGeneration;
        imageView->setImage({});
        pages->setCurrentWidget(listPage);
        if (grid->currentIndex().isValid()) {
            grid->scrollTo(grid->currentIndex());
        }
        grid->setFocus();
        show();
    }

    void requestImage()
    {
        const quint64 generation = ++viewerGeneration;
        viewerContent->setCurrentWidget(viewerState);
        viewerState->set(Icon::Refresh, QStringLiteral("Loading full-size screenshot…"), {});
        saveButton->setEnabled(false);
        copyButton->setEnabled(false);
        zoomMode->setEnabled(false);
        client.image(openedItem.id, false, &context, [this, generation](QImage image, QString error) {
            if (generation != viewerGeneration) {
                return;
            }
            if (!error.isEmpty() || image.isNull()) {
                viewerState->set(Icon::Warning, QStringLiteral("Couldn’t open this screenshot"),
                                 error.isEmpty() ? QStringLiteral("The library returned an image ScreenFrame can’t read.") : error,
                                 QStringLiteral("Try again"));
                return;
            }
            viewerContent->setCurrentWidget(imageView);
            zoomMode->setCurrent(0);
            imageView->setImage(std::move(image));
            saveButton->setEnabled(true);
            copyButton->setEnabled(true);
            zoomMode->setEnabled(true);
        });
    }

    bool imageReady() const
    {
        return pages->currentWidget() == viewerPage && saveButton->isEnabled();
    }

    void copyImage()
    {
        if (imageReady()) {
            QApplication::clipboard()->setImage(imageView->image());
            viewerToast->flash(QStringLiteral("Copied to clipboard"));
        }
    }

    void saveImage()
    {
        if (imageReady() && saveImageAs(&window, imageView->image(), openedItem.capturedAt)) {
            viewerToast->flash(QStringLiteral("Screenshot saved"));
        }
    }
};

LibraryWindow::LibraryWindow(LibraryClient &client, QWidget *parent)
    : QMainWindow(parent, Qt::Window), ui(std::make_unique<Ui>(*this, client))
{
}

LibraryWindow::~LibraryWindow() = default;

void LibraryWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    ui->needsRefresh = true;
    ui->show();
}
