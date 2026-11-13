#include "settingswindow.h"

#include "settings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QStyle>
#include <QVBoxLayout>

namespace {

QLabel *label(const QString &text, const char *style, QWidget *parent)
{
    auto *result = new QLabel(text, parent);
    result->setObjectName(QString::fromLatin1(style));
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}

constexpr auto styles = R"(
    QScrollArea, QWidget#settingsPage { background: #111421; }
    QWidget#settingsCard, QWidget#settingsFooter { background: #1b2032; }
    QWidget#settingsCard { border: 1px solid #343d56; border-radius: 9px; }
    QLabel, QCheckBox { color: #edf0ff; }
    QLabel:disabled, QCheckBox:disabled { color: #9099b5; }
    QLabel#heading { font-size: 21px; font-weight: 700; }
    QLabel#section { font-size: 16px; font-weight: 700; }
    QLabel#detail { color: #b2bbd8; }
    QLabel#settingsStatus { color: #b2bbd8; padding: 5px 0; }
    QLabel#settingsStatus[severity="error"] { color: #f2cb86; }
    QLabel#settingsStatus[severity="success"] { color: #a4dfc8; }
    QCheckBox { spacing: 8px; }
    QPushButton, QLineEdit, QComboBox, QSpinBox {
        background: #22283c; border: 1px solid #39435d; border-radius: 6px;
        padding: 6px 10px; color: #eef0ff; selection-background-color: #7053c7;
    }
    QLineEdit, QComboBox, QSpinBox { min-height: 20px; }
    QPushButton:hover, QComboBox:hover { background: #2b334d; border-color: #58658a; }
    QPushButton:focus, QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color: #c4a8ff; }
    QPushButton:disabled, QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { color: #9099b5; background: #1b2032; }
    QPushButton#primary { background: #7053c7; border-color: #8e6ce4; color: white; font-weight: 700; }
    QPushButton#primary:hover { background: #8261dd; }
    QPushButton#primary:focus { border-color: #f1e6ff; }
    QPushButton#primary:disabled { background: #312843; border-color: #4e4365; color: #a69bbd; }
    QComboBox QAbstractItemView { background: #22283c; color: #edf0ff; selection-background-color: #604a98; }
    QScrollBar:vertical { background: #181d2c; width: 12px; margin: 0; }
    QScrollBar::handle:vertical { background: #475270; border-radius: 4px; min-height: 25px; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
)";

QWidget *card(QWidget *parent, QVBoxLayout *&column)
{
    auto *panel = new QFrame(parent);
    panel->setObjectName(QStringLiteral("settingsCard"));
    column = new QVBoxLayout(panel);
    column->setContentsMargins(16, 14, 16, 14);
    column->setSpacing(8);
    return panel;
}

void addField(QFormLayout *form, const QString &title, QWidget *field, QWidget *parent)
{
    auto *caption = label(title, "fieldLabel", parent);
    caption->setWordWrap(false);
    caption->setBuddy(field);
    form->addRow(caption, field);
}

}

struct SettingsWindow::Ui {
    Ui(SettingsWindow &dialog, const AppSettings &settings) : dialog(dialog)
    {
        dialog.setWindowTitle(QStringLiteral("Settings — ScreenFrame"));
        dialog.setMinimumSize(490, 400);
        const QScreen *screen = dialog.parentWidget() ? dialog.parentWidget()->screen() : QGuiApplication::primaryScreen();
        if (screen) {
            expandedHeight = qBound(dialog.minimumHeight(), screen->availableGeometry().height() - 70, 660);
        }
        auto *root = new QVBoxLayout(&dialog);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(0);
        auto *scroll = new QScrollArea(&dialog);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *page = new QWidget(scroll);
        page->setObjectName(QStringLiteral("settingsPage"));
        auto *column = new QVBoxLayout(page);
        column->setContentsMargins(20, 17, 20, 17);
        column->setSpacing(13);
        column->addWidget(label(QStringLiteral("Settings"), "heading", page));
        column->addWidget(makeShortcutsCard(page, settings.useSystemRegionShortcut));
        column->addWidget(makeLibraryCard(page, settings.library));
        column->addStretch();
        scroll->setWidget(page);
        root->addWidget(scroll, 1);

        auto *footer = new QWidget(&dialog);
        footer->setObjectName(QStringLiteral("settingsFooter"));
        auto *row = new QHBoxLayout(footer);
        row->setContentsMargins(20, 10, 20, 10);
        row->addStretch();
        auto *buttons = new QDialogButtonBox(footer);
        buttons->addButton(QStringLiteral("Cancel"), QDialogButtonBox::RejectRole);
        save = buttons->addButton(QStringLiteral("Save Settings"), QDialogButtonBox::AcceptRole);
        save->setObjectName(QStringLiteral("primary"));
        save->setDefault(true);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &SettingsWindow::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &SettingsWindow::reject);
        row->addWidget(buttons);
        root->addWidget(footer);
        syncForm();
        dialog.setStyleSheet(QString::fromLatin1(styles));
        dialog.resize(620, connection->currentIndex() == 0 ? dialog.minimumHeight() : expandedHeight);
    }

    SettingsWindow &dialog;
    QCheckBox *takeover = nullptr;
    QComboBox *connection = nullptr;
    QLineEdit *host = nullptr;
    QSpinBox *port = nullptr;
    QLineEdit *device = nullptr;
    QLineEdit *token = nullptr;
    QCheckBox *archive = nullptr;
    QLabel *libraryHint = nullptr;
    QLabel *status = nullptr;
    QPushButton *test = nullptr;
    QPushButton *save = nullptr;
    QFormLayout *form = nullptr;
    bool testing = false;
    int expandedHeight = 660;

    QWidget *makeShortcutsCard(QWidget *page, bool useSystemRegionShortcut)
    {
        QVBoxLayout *layout;
        auto *panel = card(page, layout);
        layout->addWidget(label(QStringLiteral("Keyboard shortcuts"), "section", panel));
#if defined(Q_OS_WIN)
        const auto text = QStringLiteral("Use Win+S for region capture");
        const auto description = QStringLiteral("While ScreenFrame is running, Win+S opens region capture in place of Windows Search.");
        const auto defaults = QStringLiteral("Default shortcuts: Ctrl+Alt+Shift+1 for full screen, Ctrl+Alt+Shift+2 for region capture.");
#elif defined(Q_OS_MACOS)
        const auto text = QStringLiteral("Use Command+Shift+5 for region capture");
        const auto description = QStringLiteral("To free this shortcut, turn off Screenshot and recording options in System Settings → "
                                                "Keyboard → Keyboard Shortcuts → Screenshots.");
        const auto defaults = QStringLiteral("Default shortcuts: Command+Option+Shift+1 for full screen, "
                                             "Command+Option+Shift+2 for region capture.");
#else
        const auto text = QStringLiteral("Use Super+S for region capture");
        const auto description = QStringLiteral("If another app uses Super+S, free the shortcut in your desktop keyboard settings.");
        const auto defaults = QStringLiteral("Default shortcuts: Ctrl+Alt+Shift+1 for full screen, Ctrl+Alt+Shift+2 for region capture.");
#endif
        layout->addWidget(label(defaults, "detail", panel));
        takeover = new QCheckBox(text, panel);
        takeover->setChecked(useSystemRegionShortcut);
        layout->addWidget(takeover);
        layout->addWidget(label(description + QStringLiteral(" Restart ScreenFrame to apply this change."), "detail", panel));
        return panel;
    }

    QWidget *makeLibraryCard(QWidget *page, const LibrarySettings &settings)
    {
        QVBoxLayout *layout;
        auto *panel = card(page, layout);
        layout->addWidget(label(QStringLiteral("Shared library"), "section", panel));
        layout->addWidget(label(QStringLiteral("Capture, edit, copy, and save locally without connecting to a library."), "detail", panel));

        auto *fields = new QWidget(panel);
        form = new QFormLayout(fields);
        form->setContentsMargins(0, 0, 0, 0);
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(6);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        connection = new QComboBox(fields);
        connection->setObjectName(QStringLiteral("libraryConnection"));
        connection->addItem(QStringLiteral("No shared library"));
        connection->addItem(QStringLiteral("Library on this computer"));
        connection->addItem(QStringLiteral("Library over SSH"));
        addField(form, QStringLiteral("&Connection"), connection, fields);
        host = new QLineEdit(settings.sshHost, fields);
        host->setObjectName(QStringLiteral("libraryHost"));
        host->setPlaceholderText(QStringLiteral("Example: screenshots-server"));
        host->setMaxLength(128);
        addField(form, QStringLiteral("SSH &host"), host, fields);
        port = new QSpinBox(fields);
        port->setObjectName(QStringLiteral("libraryPort"));
        port->setRange(1, 65535);
        port->setValue(settings.port);
        port->setGroupSeparatorShown(false);
        addField(form, QStringLiteral("Library &port"), port, fields);
        device = new QLineEdit(settings.device, fields);
        device->setObjectName(QStringLiteral("libraryDevice"));
        device->setPlaceholderText(QStringLiteral("Example: Work laptop"));
        device->setMaxLength(48);
        addField(form, QStringLiteral("&Device name"), device, fields);
        token = new QLineEdit(settings.token, fields);
        token->setObjectName(QStringLiteral("libraryToken"));
        token->setEchoMode(QLineEdit::Password);
        token->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText | Qt::ImhSensitiveData);
        token->setPlaceholderText(QStringLiteral("Access token from the library server"));
        token->setMaxLength(256);
        addField(form, QStringLiteral("Access &token"), token, fields);
        layout->addWidget(fields);
        libraryHint = label({}, "detail", panel);
        layout->addWidget(libraryHint);

        archive = new QCheckBox(QStringLiteral("Automatically archive screenshots from this device"), panel);
        archive->setObjectName(QStringLiteral("libraryArchive"));
        archive->setChecked(settings.isConfigured() && settings.autoArchive);
        layout->addWidget(archive);
        auto *privacy = label(QStringLiteral("Sends the final PNG with your edits, capture time, and device name when you close "
                                             "the editor, replace the capture, or quit. Command-line captures without a preview "
                                             "are queued immediately. Turning this off pauses queued uploads."),
                              "detail", panel);
        privacy->setObjectName(QStringLiteral("archiveDetails"));
        layout->addWidget(privacy);

        auto *actions = new QHBoxLayout;
        status = label({}, "settingsStatus", panel);
        actions->addWidget(status, 1);
        test = new QPushButton(QStringLiteral("Test Connection"), panel);
        test->setAutoDefault(false);
        actions->addWidget(test, 0, Qt::AlignTop);
        layout->addLayout(actions);

        if (settings.isConfigured() || !settings.token.isEmpty() || !settings.sshHost.isEmpty()
            || !settings.device.isEmpty() || settings.autoArchive) {
            connection->setCurrentIndex(settings.transport == LibraryTransport::Ssh ? 2 : 1);
        }
        QObject::connect(connection, &QComboBox::currentIndexChanged, &dialog, [this, privacy] {
            privacy->setVisible(connection->currentIndex() != 0);
            clearStatus();
            syncForm();
            if (connection->currentIndex() != 0 && dialog.height() < expandedHeight) {
                dialog.resize(dialog.width(), expandedHeight);
            }
        });
        const auto clear = [this] { clearStatus(); };
        QObject::connect(host, &QLineEdit::textEdited, &dialog, clear);
        QObject::connect(port, &QSpinBox::valueChanged, &dialog, clear);
        QObject::connect(device, &QLineEdit::textEdited, &dialog, clear);
        QObject::connect(token, &QLineEdit::textEdited, &dialog, clear);
        QObject::connect(archive, &QCheckBox::toggled, &dialog, clear);
        QObject::connect(test, &QPushButton::clicked, &dialog, [this] { beginTest(); });
        privacy->setVisible(connection->currentIndex() != 0);
        return panel;
    }

    AppSettings settings() const
    {
        AppSettings result;
        result.useSystemRegionShortcut = takeover->isChecked();
        if (connection->currentIndex() == 0) {
            return result;
        }
        LibrarySettings &library = result.library;
        library.transport = connection->currentIndex() == 2 ? LibraryTransport::Ssh : LibraryTransport::Local;
        if (library.transport == LibraryTransport::Ssh) {
            library.sshHost = host->text().trimmed();
        }
        library.port = port->value();
        library.device = device->text().trimmed();
        library.token = token->text().trimmed();
        library.autoArchive = archive->isChecked();
        return result;
    }

    void setStatus(const QString &message, const char *severity = "")
    {
        status->setText(message);
        status->setProperty("severity", QString::fromLatin1(severity));
        status->style()->unpolish(status);
        status->style()->polish(status);
    }

    void clearStatus() { setStatus({}); }

    void syncForm()
    {
        const bool enabled = connection->currentIndex() != 0;
        const bool ssh = connection->currentIndex() == 2;
        form->setRowVisible(host, ssh);
        form->setRowVisible(port, enabled);
        form->setRowVisible(device, enabled);
        form->setRowVisible(token, enabled);
        archive->setVisible(enabled);
        test->setVisible(enabled);
        status->setVisible(enabled);
        connection->setEnabled(!testing);
        host->setEnabled(!testing);
        port->setEnabled(!testing);
        device->setEnabled(!testing);
        token->setEnabled(!testing);
        archive->setEnabled(!testing);
        test->setEnabled(!testing);
        save->setEnabled(!testing);
        libraryHint->setVisible(enabled);
        libraryHint->setText(ssh
            ? QStringLiteral("Use an SSH host or alias that already connects without prompting and has a verified host key.")
            : QStringLiteral("Connects to a library server listening on this computer’s loopback interface."));
    }

    bool validate(const LibrarySettings &library)
    {
        if (connection->currentIndex() == 0) {
            return true;
        }
        QString error;
        if (library.transport == LibraryTransport::Ssh && library.sshHost.isEmpty()) {
            error = QStringLiteral("Enter the library server’s SSH host or alias.");
            host->setFocus();
        } else if (library.device.isEmpty()) {
            error = QStringLiteral("Enter a device name.");
            device->setFocus();
        } else if (library.token.isEmpty()) {
            error = QStringLiteral("Enter the library access token.");
            token->setFocus();
        } else {
            error = library.validationError();
        }
        if (error.isEmpty() && !library.isConfigured()) {
            error = QStringLiteral("The library settings are incomplete.");
        }
        if (!error.isEmpty()) {
            setStatus(error, "error");
            return false;
        }
        return true;
    }

    void beginTest()
    {
        const auto library = settings().library;
        if (testing || connection->currentIndex() == 0 || !validate(library)) {
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

void SettingsWindow::setTestResult(const QString &message, bool success)
{
    if (!ui->testing) {
        return;
    }
    ui->testing = false;
    ui->syncForm();
    const QString fallback = success ? QStringLiteral("Connected to the library.") : QStringLiteral("Could not connect to the library.");
    QString safeMessage = message;
    const QString token = settings().library.token;
    if (!token.isEmpty()) {
        safeMessage.replace(token, QStringLiteral("[hidden]"));
    }
    ui->setStatus(safeMessage.isEmpty() ? fallback : safeMessage, success ? "success" : "error");
}
