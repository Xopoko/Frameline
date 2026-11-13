#include "librarywindow.h"

#include "libraryclient.h"
#include "pngexport.h"

#include <QAbstractListModel>
#include <QCache>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

namespace {

constexpr int thumbnailWidth = 112;
constexpr int thumbnailHeight = 64;
constexpr int rowHeight = 84;

QString itemTitle(const LibraryItem &item)
{
    if (item.kind == QStringLiteral("full")) {
        return QStringLiteral("Full screen");
    }
    if (item.kind == QStringLiteral("region")) {
        return QStringLiteral("Screen region");
    }
    return QStringLiteral("Screenshot");
}

QString itemDetails(const LibraryItem &item)
{
    const QString timestamp = QLocale(QLocale::English).toString(item.capturedAt.toLocalTime(),
                                                                QStringLiteral("d MMM yyyy, HH:mm"));
    return QStringLiteral("%1  ·  %2  ·  %3 × %4 px")
        .arg(timestamp, item.device).arg(item.size.width()).arg(item.size.height());
}

QLabel *label(const QString &text, const char *style, QWidget *parent)
{
    auto *result = new QLabel(text, parent);
    result->setObjectName(QString::fromLatin1(style));
    result->setTextFormat(Qt::PlainText);
    return result;
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
        if (role == Qt::DisplayRole) {
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

    void setThumbnail(int row, QImage image)
    {
        if (image.isNull()) {
            return;
        }
        const QSize size(thumbnailWidth * 2, thumbnailHeight * 2);
        image.setDevicePixelRatio(1);
        if (image.width() > size.width() || image.height() > size.height()) {
            image = image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        thumbnails.insert(at(row).id, new QImage(std::move(image)));
        const QModelIndex updated = index(row);
        emit dataChanged(updated, updated, {Qt::DecorationRole});
    }

private:
    QList<LibraryItem> items;
    QCache<QString, QImage> thumbnails{80};
};

class LibraryDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(320, rowHeight);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const auto *model = static_cast<const LibraryModel *>(index.model());
        const auto &item = model->at(index.row());
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool focused = option.state.testFlag(QStyle::State_HasFocus);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        const QRect card = option.rect.adjusted(1, 4, -1, -4);
        painter->setBrush(selected ? QColor("#342752") : QColor(hovered ? "#232942" : "#1b2032"));
        painter->setPen(QColor(focused ? "#c4a8ff" : selected ? "#7053a9" : "#30384f"));
        painter->drawRoundedRect(card.adjusted(0, 0, -1, -1), 8, 8);

        const QRect preview(card.left() + 7, card.center().y() - thumbnailHeight / 2,
                            thumbnailWidth, thumbnailHeight);
        painter->fillRect(preview, QColor("#111421"));
        if (const auto *thumbnail = model->thumbnail(item.id)) {
            QSize fitted = thumbnail->size();
            fitted.scale(preview.size(), Qt::KeepAspectRatio);
            QRect destination(QPoint{}, fitted);
            destination.moveCenter(preview.center());
            painter->drawImage(destination, *thumbnail);
        } else {
            QRect glyph(0, 0, 20, 14);
            glyph.moveCenter(preview.center());
            painter->setPen(QPen(QColor("#727d9d"), 1.25));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(glyph, 2, 2);
            painter->drawLine(glyph.bottomLeft() + QPoint(4, -3), glyph.center() + QPoint(1, -1));
            painter->drawLine(glyph.center() + QPoint(1, -1), glyph.bottomRight() + QPoint(-3, -3));
        }

        const int x = preview.right() + 15;
        const int textWidth = card.right() - x - 13;
        QFont titleFont(option.font);
        titleFont.setWeight(QFont::DemiBold);
        painter->setFont(titleFont);
        painter->setPen(QColor("#f1edff"));
        painter->drawText(QRect(x, card.top() + 11, textWidth, 24), Qt::AlignVCenter,
                          QFontMetrics(titleFont).elidedText(itemTitle(item), Qt::ElideRight, textWidth));
        painter->setFont(option.font);
        painter->setPen(QColor("#b2bbd8"));
        painter->drawText(QRect(x, card.top() + 37, textWidth, 23), Qt::AlignVCenter,
                          option.fontMetrics.elidedText(itemDetails(item), Qt::ElideRight, textWidth));
        painter->restore();
    }
};

class LibraryList final : public QListView {
public:
    using QListView::QListView;
    std::function<void()> resized;

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QListView::resizeEvent(event);
        if (resized) {
            resized();
        }
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
    explicit ImageView(QWidget *parent) : QScrollArea(parent), canvas(new ImageCanvas)
    {
        setFrameShape(QFrame::NoFrame);
        setAlignment(Qt::AlignCenter);
        setWidget(canvas);
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
    void resizeEvent(QResizeEvent *event) override
    {
        QScrollArea::resizeEvent(event);
        resizeCanvas();
    }

private:
    ImageCanvas *canvas;
    bool fit = true;

    void resizeCanvas()
    {
        const QSize imageSize = image().size();
        if (image().isNull()) {
            canvas->resize(1, 1);
            return;
        }
        const qreal dpr = devicePixelRatioF();
        QSize target(qMax(1, qRound(imageSize.width() / dpr)), qMax(1, qRound(imageSize.height() / dpr)));
        if (fit) {
            const QSize available = (maximumViewportSize() - QSize(28, 28)).expandedTo(QSize(1, 1));
            target.scale(available.boundedTo(target), Qt::KeepAspectRatio);
        }
        canvas->resize(target.expandedTo(QSize(1, 1)));
    }
};

constexpr auto libraryStyles = R"(
    QWidget#libraryRoot { background: #111421; }
    QWidget#libraryBar, QWidget#viewerBar { background: #191d2d; }
    QLabel { color: #edf0ff; }
    QLabel#libraryTitle { font-size: 22px; font-weight: 700; }
    QLabel#stateTitle { font-size: 18px; font-weight: 700; }
    QLabel#details, QLabel#status { color: #b2bbd8; }
    QLabel#alert { color: #f2cb86; background: #31291e; border: 1px solid #584429; border-radius: 5px; padding: 7px 10px; }
    QListView { background: #111421; color: #edf0ff; border: none; outline: none; }
    QScrollArea { background: #111421; }
    QPushButton { background: #22283c; border: 1px solid #343d56; border-radius: 6px; padding: 7px 12px; color: #eef0ff; }
    QPushButton:hover { background: #2b334d; border-color: #58658a; }
    QPushButton:focus { border-color: #c4a8ff; }
    QPushButton:disabled { color: #828ba8; background: #1c2032; }
    QPushButton#primary { background: #7053c7; border-color: #8e6ce4; color: white; font-weight: 700; }
    QPushButton#primary:hover { background: #8261dd; }
    QPushButton#primary:disabled { background: #312843; border-color: #4e4365; color: #a69bbd; }
    QPushButton#primary:focus { border-color: #f1e6ff; }
    QScrollBar:vertical { background: #181d2c; width: 12px; margin: 0; }
    QScrollBar:horizontal { background: #181d2c; height: 12px; margin: 0; }
    QScrollBar::handle { background: #475270; border-radius: 4px; min-width: 25px; min-height: 25px; }
    QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
    QScrollBar::add-page, QScrollBar::sub-page { background: none; }
)";

}

struct LibraryWindow::Ui {
    Ui(LibraryWindow &owner, LibraryClient &transport) : window(owner), client(transport)
    {
        window.setWindowTitle(QStringLiteral("Screenshot Library — ScreenFrame"));
        window.setMinimumSize(540, 400);
        window.resize(850, 660);

        auto *root = new QWidget(&window);
        root->setObjectName(QStringLiteral("libraryRoot"));
        root->setStyleSheet(QString::fromLatin1(libraryStyles));
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
        listView->resized = [this] { scheduleThumbnails(); };
        QObject::connect(listView->verticalScrollBar(), &QScrollBar::valueChanged, &context,
                         [this] { scheduleThumbnails(); });

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

        auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), &window);
        QObject::connect(escape, &QShortcut::activated, &context, [this] {
            if (pages->currentWidget() == viewerPage) {
                showList();
            } else {
                window.close();
            }
        });
        auto *refreshKey = new QShortcut(QKeySequence::Refresh, &window);
        QObject::connect(refreshKey, &QShortcut::activated, &context, [this] {
            if (pages->currentWidget() == listPage && client.isConfigured()) {
                refresh();
            }
        });
        auto *saveKey = new QShortcut(QKeySequence::Save, &window);
        QObject::connect(saveKey, &QShortcut::activated, &context, [this] { saveImage(); });
    }

    ~Ui() { listView->resized = {}; }

    LibraryWindow &window;
    LibraryClient &client;
    QStackedWidget *pages = nullptr;
    QWidget *listPage = nullptr;
    QWidget *viewerPage = nullptr;
    QStackedWidget *listContent = nullptr;
    LibraryList *listView = nullptr;
    LibraryModel *model = nullptr;
    QLabel *stateTitle = nullptr;
    QLabel *stateDetails = nullptr;
    QLabel *listAlert = nullptr;
    QLabel *syncLabel = nullptr;
    QPushButton *refreshButton = nullptr;
    QPushButton *openButton = nullptr;
    QPushButton *loadMoreButton = nullptr;
    QTimer *thumbnailTimer = nullptr;
    QTimer *refreshTimer = nullptr;
    QSet<QString> failedThumbnails;
    QSet<QString> pendingThumbnails;
    quint64 listGeneration = 0;
    bool needsRefresh = true;
    bool listing = false;
    QString nextCursor;

    QStackedWidget *viewerContent = nullptr;
    ImageView *imageView = nullptr;
    QLabel *viewerTitle = nullptr;
    QLabel *viewerDetails = nullptr;
    QLabel *viewerState = nullptr;
    QPushButton *retryImage = nullptr;
    QPushButton *saveButton = nullptr;
    QPushButton *fitButton = nullptr;
    QPushButton *actualButton = nullptr;
    LibraryItem openedItem;
    quint64 viewerGeneration = 0;
    QObject context;

    QWidget *makeListPage()
    {
        auto *page = new QWidget(pages);
        auto *column = new QVBoxLayout(page);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        auto *bar = new QWidget(page);
        bar->setObjectName(QStringLiteral("libraryBar"));
        auto *header = new QHBoxLayout(bar);
        header->setContentsMargins(18, 13, 18, 13);
        header->addWidget(label(QStringLiteral("Screenshot Library"), "libraryTitle", bar));
        header->addStretch();
        refreshButton = new QPushButton(QStringLiteral("Refresh"), bar);
        refreshButton->setToolTip(QStringLiteral("Refresh screenshots"));
        QObject::connect(refreshButton, &QPushButton::clicked, &context, [this] { refresh(); });
        header->addWidget(refreshButton);
        openButton = new QPushButton(QStringLiteral("Open"), bar);
        openButton->setObjectName(QStringLiteral("primary"));
        openButton->setEnabled(false);
        QObject::connect(openButton, &QPushButton::clicked, &context,
                         [this] { openItem(listView->currentIndex()); });
        header->addWidget(openButton);
        column->addWidget(bar);

        auto *body = new QWidget(page);
        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(18, 12, 18, 12);
        bodyLayout->setSpacing(8);
        listAlert = label({}, "alert", body);
        listAlert->setWordWrap(true);
        listAlert->hide();
        bodyLayout->addWidget(listAlert);

        listContent = new QStackedWidget(body);
        auto *statePage = new QWidget(listContent);
        auto *stateLayout = new QVBoxLayout(statePage);
        stateLayout->setContentsMargins(10, 10, 10, 10);
        stateLayout->addStretch();
        stateTitle = label(QStringLiteral("Loading screenshots…"), "stateTitle", statePage);
        stateTitle->setAlignment(Qt::AlignCenter);
        stateLayout->addWidget(stateTitle);
        stateDetails = label({}, "details", statePage);
        stateDetails->setAlignment(Qt::AlignCenter);
        stateDetails->setWordWrap(true);
        stateLayout->addWidget(stateDetails);
        stateLayout->addStretch();
        listContent->addWidget(statePage);

        listView = new LibraryList(listContent);
        listView->setAccessibleName(QStringLiteral("Screenshots"));
        listView->setFrameShape(QFrame::NoFrame);
        listView->setUniformItemSizes(true);
        listView->setMouseTracking(true);
        listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        model = new LibraryModel(listView);
        listView->setModel(model);
        listView->setItemDelegate(new LibraryDelegate(listView));
        QObject::connect(listView, &QListView::activated, &context, [this](const QModelIndex &index) { openItem(index); });
        QObject::connect(listView->selectionModel(), &QItemSelectionModel::currentChanged, &context,
                         [this] { openButton->setEnabled(listView->currentIndex().isValid()); });
        listContent->addWidget(listView);
        bodyLayout->addWidget(listContent, 1);

        auto *footer = new QHBoxLayout;
        syncLabel = label({}, "status", body);
        syncLabel->setWordWrap(true);
        footer->addWidget(syncLabel, 1);
        loadMoreButton = new QPushButton(QStringLiteral("Load more"), body);
        loadMoreButton->hide();
        QObject::connect(loadMoreButton, &QPushButton::clicked, &context, [this] { requestList(nextCursor); });
        footer->addWidget(loadMoreButton);
        bodyLayout->addLayout(footer);
        column->addWidget(body, 1);
        return page;
    }

    QWidget *makeViewerPage()
    {
        auto *page = new QWidget(pages);
        auto *column = new QVBoxLayout(page);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(0);
        auto *bar = new QWidget(page);
        bar->setObjectName(QStringLiteral("viewerBar"));
        auto *row = new QHBoxLayout(bar);
        row->setContentsMargins(16, 10, 16, 10);
        auto *back = new QPushButton(QStringLiteral("Back to library"), bar);
        QObject::connect(back, &QPushButton::clicked, &context, [this] { showList(); });
        row->addWidget(back);
        row->addSpacing(6);
        auto *heading = new QVBoxLayout;
        heading->setSpacing(2);
        viewerTitle = label({}, "stateTitle", bar);
        viewerDetails = label({}, "details", bar);
        viewerDetails->setWordWrap(true);
        heading->addWidget(viewerTitle);
        heading->addWidget(viewerDetails);
        row->addLayout(heading, 1);
        fitButton = new QPushButton(QStringLiteral("Fit"), bar);
        QObject::connect(fitButton, &QPushButton::clicked, &context, [this] { imageView->setFit(true); });
        row->addWidget(fitButton);
        actualButton = new QPushButton(QStringLiteral("1:1"), bar);
        actualButton->setToolTip(QStringLiteral("Actual size"));
        QObject::connect(actualButton, &QPushButton::clicked, &context, [this] { imageView->setFit(false); });
        row->addWidget(actualButton);
        saveButton = new QPushButton(QStringLiteral("Save PNG"), bar);
        saveButton->setObjectName(QStringLiteral("primary"));
        QObject::connect(saveButton, &QPushButton::clicked, &context, [this] { saveImage(); });
        row->addWidget(saveButton);
        column->addWidget(bar);

        viewerContent = new QStackedWidget(page);
        auto *statePage = new QWidget(viewerContent);
        auto *stateLayout = new QVBoxLayout(statePage);
        stateLayout->setContentsMargins(18, 18, 18, 18);
        stateLayout->addStretch();
        viewerState = label({}, "details", statePage);
        viewerState->setWordWrap(true);
        viewerState->setAlignment(Qt::AlignCenter);
        stateLayout->addWidget(viewerState);
        retryImage = new QPushButton(QStringLiteral("Try again"), statePage);
        QObject::connect(retryImage, &QPushButton::clicked, &context, [this] { requestImage(); });
        stateLayout->addWidget(retryImage, 0, Qt::AlignHCenter);
        stateLayout->addStretch();
        viewerContent->addWidget(statePage);
        imageView = new ImageView(viewerContent);
        viewerContent->addWidget(imageView);
        column->addWidget(viewerContent, 1);
        return page;
    }

    void show()
    {
        if (needsRefresh && !listing && pages->currentWidget() == listPage) {
            refresh();
        }
        scheduleThumbnails();
    }

    void setListState(const QString &title, const QString &details = {})
    {
        stateTitle->setText(title);
        stateDetails->setText(details);
        listContent->setCurrentIndex(0);
    }

    void refresh()
    {
        failedThumbnails.clear();
        if (!client.isConfigured()) {
            needsRefresh = true;
            refreshButton->setEnabled(false);
            setListState(QStringLiteral("Library is not set up"),
                         QStringLiteral("See the ScreenFrame README for library setup instructions."));
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
        loadMoreButton->setEnabled(false);
        listAlert->hide();
        if (first && model->rowCount() == 0) {
            setListState(QStringLiteral("Loading screenshots…"));
        }
        client.list(cursor, &context, [this, first](QList<LibraryItem> entries, QString next, QString error) {
            listing = false;
            refreshButton->setEnabled(true);
            loadMoreButton->setEnabled(true);
            if (!error.isEmpty()) {
                if (model->rowCount() == 0) {
                    setListState(QStringLiteral("Could not load screenshots"), error);
                } else {
                    listAlert->setText(error);
                    listAlert->show();
                }
                return;
            }
            nextCursor = std::move(next);
            loadMoreButton->setVisible(!nextCursor.isEmpty());
            if (first) {
                const QModelIndex current = listView->currentIndex();
                const QString selectedId = current.isValid() ? model->at(current.row()).id : QString();
                ++listGeneration;
                model->replace(std::move(entries));
                listView->scrollToTop();
                if (!selectedId.isEmpty()) {
                    for (int row = 0; row < model->rowCount(); ++row) {
                        if (model->at(row).id == selectedId) {
                            listView->setCurrentIndex(model->index(row));
                            break;
                        }
                    }
                }
            } else {
                model->append(std::move(entries));
            }
            if (model->rowCount() == 0) {
                setListState(QStringLiteral("No screenshots yet"),
                             QStringLiteral("Take a screenshot on a device connected to this library."));
            } else {
                listContent->setCurrentWidget(listView);
            }
            openButton->setEnabled(listView->currentIndex().isValid());
            if (needsRefresh) {
                refreshTimer->start();
            }
            scheduleThumbnails();
        });
    }

    void scheduleThumbnails()
    {
        if (thumbnailTimer && window.isVisible() && pages->currentWidget() == listPage && listView->isVisible()) {
            thumbnailTimer->start();
        }
    }

    void loadThumbnails()
    {
        if (!window.isVisible() || pages->currentWidget() != listPage || !listView->isVisible()) {
            return;
        }
        constexpr int maxRequests = 4;
        const QModelIndex firstVisible = listView->indexAt(QPoint(4, 1));
        if (!firstVisible.isValid()) {
            return;
        }
        const int first = std::max(0, firstVisible.row() - 1);
        const int last = std::min(model->rowCount(), first + listView->viewport()->height() / rowHeight + 4);
        for (int row = first; row < last && pendingThumbnails.size() < maxRequests; ++row) {
            const LibraryItem &item = model->at(row);
            if (!item.hasThumbnail || model->thumbnail(item.id) || failedThumbnails.contains(item.id)
                || pendingThumbnails.contains(item.id)) {
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
                        failedThumbnails.insert(id);
                    }
                }
                scheduleThumbnails();
            });
        }
    }

    void updateSync(int pending, const QString &error)
    {
        if (!error.isEmpty()) {
            syncLabel->setText(QStringLiteral("Sync paused: %1").arg(error));
        } else if (pending > 0) {
            syncLabel->setText(QStringLiteral("Waiting to sync: %1").arg(pending));
        } else {
            syncLabel->clear();
        }
    }

    void openItem(const QModelIndex &index)
    {
        if (!index.isValid() || index.row() >= model->rowCount()) {
            return;
        }
        openedItem = model->at(index.row());
        viewerTitle->setText(itemTitle(openedItem));
        viewerDetails->setText(itemDetails(openedItem));
        pages->setCurrentWidget(viewerPage);
        requestImage();
    }

    void showList()
    {
        ++viewerGeneration;
        imageView->setImage({});
        pages->setCurrentWidget(listPage);
        listView->setFocus();
        show();
    }

    void requestImage()
    {
        const quint64 generation = ++viewerGeneration;
        viewerContent->setCurrentIndex(0);
        viewerState->setText(QStringLiteral("Loading full-size screenshot…"));
        retryImage->hide();
        saveButton->setEnabled(false);
        fitButton->setEnabled(false);
        actualButton->setEnabled(false);
        client.image(openedItem.id, false, &context, [this, generation](QImage image, QString error) {
            if (generation != viewerGeneration) {
                return;
            }
            if (!error.isEmpty() || image.isNull()) {
                viewerState->setText(error.isEmpty() ? QStringLiteral("Could not open screenshot.") : error);
                retryImage->show();
                return;
            }
            viewerContent->setCurrentWidget(imageView);
            imageView->setImage(std::move(image));
            saveButton->setEnabled(true);
            fitButton->setEnabled(true);
            actualButton->setEnabled(true);
        });
    }

    void saveImage()
    {
        if (pages->currentWidget() != viewerPage || !saveButton->isEnabled()) {
            return;
        }
        const QImage image = imageView->image();
        QString directory = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        if (directory.isEmpty() || !QDir(directory).exists()) {
            directory = QDir::homePath();
        }
        QFileDialog dialog(&window, QStringLiteral("Save screenshot"), directory, QStringLiteral("PNG (*.png)"));
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix(QStringLiteral("png"));
        const QString timestamp = openedItem.capturedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
        dialog.selectFile(QStringLiteral("Screenshot_%1.png").arg(timestamp));
        if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) {
            return;
        }
        QString error;
        if (!savePng(image, dialog.selectedFiles().constFirst(), &error)) {
            QMessageBox::warning(&window, QStringLiteral("Could not save screenshot"), error);
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
