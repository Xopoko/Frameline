#include "settingswindow.h"

#include "controls.h"
#include "settings.h"
#include "theme.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QIntValidator>
#include <QVBoxLayout>

#include <algorithm>

namespace {

constexpr auto settingsRules = R"(
    QLabel#sectionTitle { font-size: 15px; font-weight: 600; }
    QLabel#rowLabel { font-size: 13px; }
    QWidget#footer { background: @surface; border-top: 1px solid @border; }
)";

#if defined(Q_OS_WIN)
const auto fullKeys = QStringLiteral("Ctrl+Alt+Shift+1");
const auto regionKeys = QStringLiteral("Ctrl+Alt+Shift+2");
const auto systemKeys = QStringLiteral("Win+S");
const auto takeoverText = QStringLiteral("Use Win+S for region capture");
const auto takeoverDetails = QStringLiteral("While ScreenFrame is running, Win+S opens region capture in place of Windows Search.");
#elif defined(Q_OS_MACOS)
const auto fullKeys = QStringLiteral("⌘⌥⇧1");
const auto regionKeys = QStringLiteral("⌘⌥⇧2");
const auto systemKeys = QStringLiteral("⌘⇧5");
const auto takeoverText = QStringLiteral("Use ⌘⇧5 for region capture");
const auto takeoverDetails = QStringLiteral("First turn off Screenshot and recording options in System Settings → Keyboard → "
                                            "Keyboard Shortcuts → Screenshots.");
#else
const auto fullKeys = QStringLiteral("Ctrl+Alt+Shift+1");
const auto regionKeys = QStringLiteral("Ctrl+Alt+Shift+2");
const auto systemKeys = QStringLiteral("Super+S");
const auto takeoverText = QStringLiteral("Use Super+S for region capture");
const auto takeoverDetails = QStringLiteral("If another app uses Super+S, free it in your desktop’s keyboard settings.");
#endif

QLabel *paragraph(const QString &text, const char *role, QWidget *parent)
{
    auto *result = theme::label(text, role, parent);
    result->setWordWrap(true);
    return result;
}

// Rows on one rounded surface, separated by hairlines, like the grouped lists of system settings.
class Group final : public QWidget {
public:
    explicit Group(QWidget *parent) : QWidget(parent), column(new QVBoxLayout(this))
    {
        column->setContentsMargins(16, 2, 16, 2);
        column->setSpacing(0);
    }

    QWidget *addRow(const QString &title, QWidget *field)
    {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 9, 0, 9);
        layout->setSpacing(16);
        auto *caption = theme::label(title, "rowLabel", row);
        caption->setFixedWidth(118);
        caption->setBuddy(field);
        layout->addWidget(caption);
        field->setParent(row);
        layout->addWidget(field, 1);
        column->addWidget(row);
        return row;
    }

    QWidget *addKeyRow(const QString &title, QWidget *keys)
    {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 12, 0, 12);
        layout->addWidget(theme::label(title, "rowLabel", row), 1);
        keys->setParent(row);
        layout->addWidget(keys);
        column->addWidget(row);
        return row;
    }

    void addWidgetRow(QWidget *widget)
    {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 8, 0, 8);
        widget->setParent(row);
        layout->addWidget(widget);
        column->addWidget(row);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const theme::Palette &c = theme::colors();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(c.border, 1));
        painter.setBrush(c.raised);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
        QList<QWidget *> rows;
        for (int index = 0; index < column->count(); ++index) {
            QWidget *row = column->itemAt(index)->widget();
            if (row && row->isVisible()) {
                rows << row;
            }
        }
        for (qsizetype index = 0; index + 1 < rows.size(); ++index) {
            painter.fillRect(QRectF(16, rows[index]->geometry().bottom() + 0.5, width() - 32, 1), c.border);
        }
    }

private:
    QVBoxLayout *column;
};

class Keycap final : public QWidget {
public:
    explicit Keycap(const QString &keys, QWidget *parent = nullptr) : QWidget(parent) { setKeys(keys); }

    void setKeys(const QString &value)
    {
        keys = value;
        setAccessibleName(value);
        setFixedSize(theme::keycapSize(keys).expandedTo(QSize(1, 1)));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        theme::paintKeycap(painter, QRectF(rect()), keys);
    }

private:
    QString keys;
};

class StatusLine final : public QWidget {
public:
    enum Severity { Neutral, Success, Error };

    explicit StatusLine(QWidget *parent) : QWidget(parent), text(theme::label({}, "body", this))
    {
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(24, 0, 0, 0);
        text->setWordWrap(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(text);
    }

    void set(const QString &message, Severity value)
    {
        severity = value;
        text->setText(message);
        setVisible(!message.isEmpty());
        update();
    }

    QString message() const { return text->text(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const theme::Palette &c = theme::colors();
        QPainter painter(this);
        const Icon icon = severity == Success ? Icon::Check : severity == Error ? Icon::Warning : Icon::Refresh;
        const QColor color = severity == Success ? c.accent : severity == Error ? c.danger : c.textMuted;
        const int line = text->fontMetrics().height();
        paintIcon(painter, icon, QRectF(0, (line - 16) / 2.0, 16, 16), color, 1.8);
    }

private:
    QLabel *text;
    Severity severity = Neutral;
};

} // namespace

struct SettingsWindow::Ui {
    Ui(SettingsWindow &dialog, const AppSettings &settings) : dialog(dialog)
    {
        dialog.setWindowTitle(QStringLiteral("Settings — ScreenFrame"));
        dialog.setMinimumWidth(540);
        dialog.setObjectName(QStringLiteral("surface"));
        auto *root = new QVBoxLayout(&dialog);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        scroll = new QScrollArea(&dialog);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        page = new QWidget(scroll);
        page->setObjectName(QStringLiteral("surface"));
        page->setAttribute(Qt::WA_StyledBackground);
        auto *column = new QVBoxLayout(page);
        column->setContentsMargins(28, 24, 28, 28);
        column->setSpacing(0);
        column->addWidget(theme::label(QStringLiteral("Settings"), "title", page));
        column->addSpacing(24);
        addShortcuts(column, settings.useSystemRegionShortcut);
        column->addSpacing(32);
        addLibrary(column, settings.library);
        column->addStretch();
        scroll->setWidget(page);
        root->addWidget(scroll, 1);
        root->addWidget(makeFooter());

        dialog.setAttribute(Qt::WA_StyledBackground);
        theme::attach(&dialog, settingsRules);
        syncForm();
        dialog.resize(600, 10);
        fitHeight();
    }

    SettingsWindow &dialog;
    QScrollArea *scroll = nullptr;
    QWidget *page = nullptr;
    Toggle *takeover = nullptr;
    Keycap *regionCap = nullptr;
    Segmented *connection = nullptr;
    QWidget *libraryDetails = nullptr;
    QWidget *hostRow = nullptr;
    QLineEdit *host = nullptr;
    QLineEdit *port = nullptr;
    QLineEdit *device = nullptr;
    QLineEdit *token = nullptr;
    Toggle *archive = nullptr;
    QLabel *libraryHint = nullptr;
    StatusLine *status = nullptr;
    Button *test = nullptr;
    Button *save = nullptr;
    bool testing = false;

    void addShortcuts(QVBoxLayout *column, bool useSystemRegionShortcut)
    {
        column->addWidget(theme::label(QStringLiteral("Capture shortcuts"), "sectionTitle", page));
        column->addSpacing(12);
        auto *group = new Group(page);
        regionCap = new Keycap(useSystemRegionShortcut ? systemKeys : regionKeys);
        group->addKeyRow(QStringLiteral("Capture a region"), regionCap);
        group->addKeyRow(QStringLiteral("Capture the screen"), new Keycap(fullKeys));
        takeover = new Toggle(takeoverText);
        takeover->setChecked(useSystemRegionShortcut);
        group->addWidgetRow(takeover);
        column->addWidget(group);
        column->addSpacing(8);
        column->addWidget(paragraph(takeoverDetails + QStringLiteral(" Changes apply after ScreenFrame restarts."), "caption", page));
        QObject::connect(takeover, &QAbstractButton::toggled, &dialog,
                         [this](bool checked) { regionCap->setKeys(checked ? systemKeys : regionKeys); });
    }

    void addLibrary(QVBoxLayout *column, const LibrarySettings &settings)
    {
        column->addWidget(theme::label(QStringLiteral("Shared library"), "sectionTitle", page));
        column->addSpacing(4);
        column->addWidget(paragraph(QStringLiteral("Collect screenshots from all your devices in a library you host. "
                                                   "Capturing, editing and saving never need one."), "muted", page));
        column->addSpacing(14);
        connection = new Segmented({QStringLiteral("Off"), QStringLiteral("This computer"), QStringLiteral("Over SSH")}, page);
        connection->setObjectName(QStringLiteral("libraryConnection"));
        connection->setAccessibleName(QStringLiteral("Library connection"));
        column->addWidget(connection);
        column->addSpacing(16);

        libraryDetails = new QWidget(page);
        auto *details = new QVBoxLayout(libraryDetails);
        details->setContentsMargins(0, 0, 0, 0);
        details->setSpacing(0);
        auto *fields = new Group(libraryDetails);
        host = new QLineEdit(settings.sshHost);
        host->setObjectName(QStringLiteral("libraryHost"));
        host->setPlaceholderText(QStringLiteral("screenshots-server"));
        host->setMaxLength(128);
        hostRow = fields->addRow(QStringLiteral("SSH host"), host);
        port = new QLineEdit(QString::number(settings.port));
        port->setObjectName(QStringLiteral("libraryPort"));
        port->setValidator(new QIntValidator(1, 65535, port));
        port->setPlaceholderText(QString::number(defaultLibraryPort));
        port->setMaxLength(5);
        port->setFixedWidth(120);
        auto *portField = new QWidget;
        auto *portLayout = new QHBoxLayout(portField);
        portLayout->setContentsMargins(0, 0, 0, 0);
        portLayout->addWidget(port);
        portLayout->addStretch();
        fields->addRow(QStringLiteral("Port"), portField);
        device = new QLineEdit(settings.device);
        device->setObjectName(QStringLiteral("libraryDevice"));
        device->setPlaceholderText(QStringLiteral("Work laptop"));
        device->setMaxLength(48);
        fields->addRow(QStringLiteral("Device name"), device);
        token = new QLineEdit(settings.token);
        token->setObjectName(QStringLiteral("libraryToken"));
        token->setEchoMode(QLineEdit::Password);
        token->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText | Qt::ImhSensitiveData);
        token->setPlaceholderText(QStringLiteral("From the library server"));
        token->setMaxLength(256);
        fields->addRow(QStringLiteral("Access token"), token);
        details->addWidget(fields);
        details->addSpacing(8);
        libraryHint = paragraph({}, "caption", libraryDetails);
        details->addWidget(libraryHint);
        details->addSpacing(20);

        auto *archiveGroup = new Group(libraryDetails);
        archive = new Toggle(QStringLiteral("Archive screenshots from this device"));
        archive->setObjectName(QStringLiteral("libraryArchive"));
        archive->setChecked(settings.isConfigured() && settings.autoArchive);
        archiveGroup->addWidgetRow(archive);
        details->addWidget(archiveGroup);
        details->addSpacing(8);
        details->addWidget(paragraph(QStringLiteral("Uploads the final PNG with your edits, the capture time and the device "
                                                    "name when you close the editor, take another capture or quit. "
                                                    "Command-line captures without a preview upload right away. Turning "
                                                    "this off pauses uploads that are waiting."), "caption", libraryDetails));
        details->addSpacing(20);

        auto *actions = new QHBoxLayout;
        actions->setSpacing(14);
        test = new Button(Button::Tonal, Icon::Refresh, QStringLiteral("Test connection"), libraryDetails);
        test->setAutoDefault(false);
        actions->addWidget(test, 0, Qt::AlignTop);
        status = new StatusLine(libraryDetails);
        status->hide();
        auto *statusColumn = new QVBoxLayout;
        statusColumn->setContentsMargins(0, 7, 0, 0);
        statusColumn->addWidget(status);
        actions->addLayout(statusColumn, 1);
        details->addLayout(actions);
        column->addWidget(libraryDetails);

        if (settings.isConfigured() || !settings.token.isEmpty() || !settings.sshHost.isEmpty()
            || !settings.device.isEmpty() || settings.autoArchive) {
            connection->setCurrent(settings.transport == LibraryTransport::Ssh ? 2 : 1);
        }
        QObject::connect(connection, &Segmented::currentChanged, &dialog, [this] {
            clearStatus();
            syncForm();
            fitHeight();
        });
        const auto clear = [this] { clearStatus(); };
        QObject::connect(host, &QLineEdit::textEdited, &dialog, clear);
        QObject::connect(port, &QLineEdit::textEdited, &dialog, clear);
        QObject::connect(device, &QLineEdit::textEdited, &dialog, clear);
        QObject::connect(token, &QLineEdit::textEdited, &dialog, clear);
        QObject::connect(archive, &QAbstractButton::toggled, &dialog, clear);
        QObject::connect(test, &QPushButton::clicked, &dialog, [this] { beginTest(); });
    }

    QWidget *makeFooter()
    {
        auto *footer = new QWidget(&dialog);
        footer->setObjectName(QStringLiteral("footer"));
        footer->setAttribute(Qt::WA_StyledBackground);
        auto *row = new QHBoxLayout(footer);
        row->setContentsMargins(28, 14, 28, 16);
        row->setSpacing(8);
        row->addStretch();
        auto *cancel = new Button(Button::Tonal, Icon::None, QStringLiteral("Cancel"), footer);
        cancel->setAutoDefault(false);
        save = new Button(Button::Primary, Icon::None, QStringLiteral("Save"), footer);
        save->setDefault(true);
        cancel->setFixedWidth(std::max(cancel->sizeHint().width(), 88));
        save->setFixedWidth(std::max(save->sizeHint().width(), 88));
        QObject::connect(cancel, &QPushButton::clicked, &dialog, &SettingsWindow::reject);
        QObject::connect(save, &QPushButton::clicked, &dialog, &SettingsWindow::accept);
        row->addWidget(cancel);
        row->addWidget(save);
        return footer;
    }

    void fitHeight()
    {
        page->adjustSize();
        const int wanted = page->sizeHint().height() + 70;
        const QScreen *screen = dialog.screen() ? dialog.screen() : QGuiApplication::primaryScreen();
        const int limit = screen ? screen->availableGeometry().height() - 80 : 760;
        dialog.resize(dialog.width(), std::clamp(wanted, 360, std::max(360, limit)));
    }

    AppSettings settings() const
    {
        AppSettings result;
        result.useSystemRegionShortcut = takeover->isChecked();
        if (connection->current() == 0) {
            return result;
        }
        LibrarySettings &library = result.library;
        library.transport = connection->current() == 2 ? LibraryTransport::Ssh : LibraryTransport::Local;
        if (library.transport == LibraryTransport::Ssh) {
            library.sshHost = host->text().trimmed();
        }
        library.port = port->text().trimmed().isEmpty() ? defaultLibraryPort : port->text().toInt();
        library.device = device->text().trimmed();
        library.token = token->text().trimmed();
        library.autoArchive = archive->isChecked();
        return result;
    }

    void setStatus(const QString &message, StatusLine::Severity severity = StatusLine::Neutral)
    {
        status->set(message, severity);
    }

    void clearStatus() { setStatus({}); }

    void syncForm()
    {
        const bool enabled = connection->current() != 0;
        const bool ssh = connection->current() == 2;
        libraryDetails->setVisible(enabled);
        hostRow->setVisible(ssh);
        for (QWidget *control : {static_cast<QWidget *>(connection), static_cast<QWidget *>(host),
                                 static_cast<QWidget *>(port), static_cast<QWidget *>(device),
                                 static_cast<QWidget *>(token), static_cast<QWidget *>(archive),
                                 static_cast<QWidget *>(test), static_cast<QWidget *>(save)}) {
            control->setEnabled(!testing);
        }
        libraryHint->setText(ssh
            ? QStringLiteral("Use an SSH host or alias that already connects without prompting and has a verified host key.")
            : QStringLiteral("Connects to a library server listening on this computer’s loopback interface."));
    }

    bool validate(const LibrarySettings &library)
    {
        if (connection->current() == 0) {
            return true;
        }
        QString error;
        if (library.transport == LibraryTransport::Ssh && library.sshHost.isEmpty()) {
            error = QStringLiteral("Enter the library server’s SSH host or alias.");
            host->setFocus();
        } else if (library.port < 1 || library.port > 65535) {
            error = QStringLiteral("Enter a port between 1 and 65535.");
            port->setFocus();
        } else if (library.device.isEmpty()) {
            error = QStringLiteral("Enter a name for this device.");
            device->setFocus();
        } else if (library.token.isEmpty()) {
            error = QStringLiteral("Enter the access token from the library server.");
            token->setFocus();
        } else {
            error = library.validationError();
        }
        if (error.isEmpty() && !library.isConfigured()) {
            error = QStringLiteral("The library settings are incomplete.");
        }
        if (!error.isEmpty()) {
            setStatus(error, StatusLine::Error);
            scroll->ensureWidgetVisible(status);
            return false;
        }
        return true;
    }

    void beginTest()
    {
        const auto library = settings().library;
        if (testing || connection->current() == 0 || !validate(library)) {
            return;
        }
        testing = true;
        syncForm();
        setStatus(QStringLiteral("Testing connection…"));
        emit dialog.testRequested(library);
    }
};

SettingsWindow::SettingsWindow(const AppSettings &settings, QWidget *parent)
    : QDialog(parent), ui(std::make_unique<Ui>(*this, settings))
{
}

SettingsWindow::~SettingsWindow() = default;

AppSettings SettingsWindow::settings() const
{
    return ui->settings();
}

void SettingsWindow::accept()
{
    if (!ui->testing && ui->validate(settings().library)) {
        QDialog::accept();
    }
}

void SettingsWindow::setTestResult(const QString &error)
{
    if (!ui->testing) {
        return;
    }
    ui->testing = false;
    ui->syncForm();
    if (error.isEmpty()) {
        ui->setStatus(QStringLiteral("Connected. This device can reach the library."), StatusLine::Success);
    } else {
        ui->setStatus(error, StatusLine::Error);
    }
}
