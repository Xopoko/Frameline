#include "settingswindow.h"

#include "controls.h"
#include "platform/autostart.h"
#include "settings.h"
#include "shortcuts.h"
#include "tailscale.h"
#include "theme.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace {

constexpr auto settingsRules = R"(
    QWidget#footer { background: @surface; border-top: 1px solid @border; }
)";

QString regionKeys(bool system)
{
    return GlobalShortcuts::defaultKeys(true, system ? RegionShortcutPreset::System : RegionShortcutPreset::Standard);
}

// What moving region capture to the system key involves on this platform.
QString takeoverDetails()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("While Frameline is running, %1 opens region capture in place of Windows Search.").arg(regionKeys(true));
#elif defined(Q_OS_MACOS)
    return QStringLiteral("First turn off Screenshot and recording options in System Settings → Keyboard → "
                          "Keyboard Shortcuts → Screenshots.");
#else
    return QStringLiteral("If another app uses %1, free it in your desktop’s keyboard settings.").arg(regionKeys(true));
#endif
}

QLabel *paragraph(const QString &text, const char *role, QWidget *parent)
{
    auto *result = theme::label(text, role, parent);
    result->setWordWrap(true);
    return result;
}

void maskSecretField(QLineEdit &field)
{
    field.setEchoMode(QLineEdit::Password);
    field.setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoAutoUppercase
                              | Qt::ImhNoPredictiveText | Qt::ImhSensitiveData);
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
        auto *caption = theme::label(title, "body", row);
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
        layout->addWidget(theme::label(title, "body", row), 1);
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
    enum Mode { Off, Host, Connect, Advanced };

    Ui(SettingsWindow &dialog, const AppSettings &settings)
        : dialog(dialog), saved(settings), hostDraft(settings.hostedLibrary)
    {
        dialog.setWindowTitle(QStringLiteral("Settings — Frameline"));
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
        addLibrary(column);
        column->addSpacing(28);
        addStartup(column);
        column->addSpacing(28);
        addCapture(column);
        column->addStretch();
        scroll->setWidget(page);
        root->addWidget(scroll, 1);
        root->addWidget(makeFooter());

        dialog.setAttribute(Qt::WA_StyledBackground);
        theme::attach(&dialog, settingsRules);
        syncForm();
        dialog.resize(600, 10);
        fitHeight();
        connection->setFocus();
    }

    SettingsWindow &dialog;
    AppSettings saved;
    HostedLibrarySettings hostDraft;
    TailscaleStatus tailscale;
    Mode selectedMode = Off;
    std::array<bool, 4> archiveDraft{false, true, true, false};
    QScrollArea *scroll = nullptr;
    QWidget *page = nullptr;
    Toggle *autostart = nullptr;
    Toggle *autoCopyScreenshot = nullptr;
    Toggle *takeover = nullptr;
    Keycap *regionCap = nullptr;
    Segmented *connection = nullptr;
    QLabel *hostOfflineNote = nullptr;
    QWidget *libraryDetails = nullptr;
    QWidget *hostDetails = nullptr;
    StatusLine *hostStatus = nullptr;
    Toggle *share = nullptr;
    QWidget *joinDetails = nullptr;
    QLineEdit *code = nullptr;
    QWidget *tailscaleDetails = nullptr;
    StatusLine *tailscaleStatus = nullptr;
    Button *downloadTailscale = nullptr;
    Button *copyCode = nullptr;
    QWidget *manualDetails = nullptr;
    Segmented *manualTransport = nullptr;
    QWidget *sshHostRow = nullptr;
    QLineEdit *sshHost = nullptr;
    QLineEdit *manualPort = nullptr;
    QLineEdit *device = nullptr;
    QLineEdit *token = nullptr;
    Toggle *archive = nullptr;
    StatusLine *status = nullptr;
    Button *test = nullptr;
    Button *cancel = nullptr;
    Button *save = nullptr;
    bool startupEditable = false;
    bool hosting = false;
    QString shareAddress;
    QString hostError;
    bool testing = false;
    std::optional<AppSettings> pendingConnection;
    bool saving = false;
    bool closeAfterSave = true;

    void addStartup(QVBoxLayout *column)
    {
        column->addWidget(theme::label(QStringLiteral("Startup"), "heading", page));
        column->addSpacing(10);
        auto *group = new Group(page);
        autostart = new Toggle(QStringLiteral("Start Frameline when I sign in"));
        autostart->setObjectName(QStringLiteral("startOnLogin"));
        QString error;
        autostart->setChecked(Autostart::isEnabled(&error));
        startupEditable = error.isEmpty();
        group->addWidgetRow(autostart);
        column->addWidget(group);
        column->addSpacing(7);
        column->addWidget(paragraph(startupEditable
            ? QStringLiteral("Frameline starts in the system tray, ready to capture and run your library.")
            : error, "caption", page));
    }

    void addCapture(QVBoxLayout *column)
    {
        column->addWidget(theme::label(QStringLiteral("Capture"), "heading", page));
        column->addSpacing(12);
        auto *group = new Group(page);
        autoCopyScreenshot = new Toggle(QStringLiteral("Copy screenshots to clipboard"));
        autoCopyScreenshot->setObjectName(QStringLiteral("autoCopyScreenshot"));
        autoCopyScreenshot->setChecked(saved.autoCopyScreenshot);
        group->addWidgetRow(autoCopyScreenshot);
        regionCap = new Keycap(regionKeys(saved.useSystemRegionShortcut));
        group->addKeyRow(QStringLiteral("Capture a region"), regionCap);
        group->addKeyRow(QStringLiteral("Capture the screen"),
                         new Keycap(GlobalShortcuts::defaultKeys(false, RegionShortcutPreset::Standard)));
        takeover = new Toggle(QStringLiteral("Use %1 for region capture").arg(regionKeys(true)));
        takeover->setChecked(saved.useSystemRegionShortcut);
        group->addWidgetRow(takeover);
        column->addWidget(group);
        column->addSpacing(8);
        column->addWidget(paragraph(takeoverDetails() + QStringLiteral(" Changes apply after Frameline restarts."), "caption", page));
        QObject::connect(takeover, &QAbstractButton::toggled, &dialog,
                         [this](bool checked) { regionCap->setKeys(regionKeys(checked)); });
    }

    void addLibrary(QVBoxLayout *column)
    {
        column->addWidget(theme::label(QStringLiteral("Shared library"), "heading", page));
        column->addSpacing(4);
        column->addWidget(paragraph(QStringLiteral("Keep screenshots from your devices together on a computer you choose."),
                                    "muted", page));
        column->addSpacing(14);
        connection = new Segmented({QStringLiteral("Off"), QStringLiteral("Create here"),
                                    QStringLiteral("Connect"), QStringLiteral("Advanced")}, page);
        connection->setObjectName(QStringLiteral("libraryConnection"));
        connection->setAccessibleName(QStringLiteral("Library connection"));
        column->addWidget(connection);
        column->addSpacing(14);
        hostOfflineNote = paragraph(QStringLiteral("Saving this choice will stop access to the library on this computer. "
                                                   "Its screenshots will stay on disk."), "caption", page);
        column->addWidget(hostOfflineNote);
        column->addWidget(makeHostDetails());
        column->addWidget(makeJoinDetails());
        column->addWidget(makeTailscaleDetails());
        column->addWidget(makeManualDetails());

        auto *fields = new Group(page);
        libraryDetails = fields;
        device = new QLineEdit(saved.library.device.isEmpty() ? defaultLibraryDeviceName() : saved.library.device);
        device->setObjectName(QStringLiteral("libraryDevice"));
        device->setPlaceholderText(QStringLiteral("Work laptop"));
        device->setMaxLength(48);
        fields->addRow(QStringLiteral("Device name"), device);
        archive = new Toggle(QStringLiteral("Archive screenshots from this device"));
        archive->setObjectName(QStringLiteral("libraryArchive"));
        archive->setToolTip(QStringLiteral("Adds the final screenshot to the library when you close it or take another."));
        fields->addWidgetRow(archive);
        column->addWidget(fields);

        status = new StatusLine(page);
        status->hide();
        auto *statusRow = new QHBoxLayout;
        statusRow->setContentsMargins(0, 8, 0, 0);
        test = new Button(Button::Tonal, Icon::Refresh, QStringLiteral("Test connection"), page);
        test->setAutoDefault(false);
        statusRow->addWidget(test, 0, Qt::AlignTop);
        statusRow->addWidget(status, 1, Qt::AlignVCenter);
        column->addLayout(statusRow);

        selectedMode = savedMode();
        archiveDraft[selectedMode] = saved.library.autoArchive;
        connection->setCurrent(selectedMode);
        archive->setChecked(saved.library.autoArchive);
        if (selectedMode == Host)
            prepareHost();
        QObject::connect(connection, &Segmented::currentChanged, &dialog, [this](int index) {
            archiveDraft[selectedMode] = archive->isChecked();
            selectedMode = static_cast<Mode>(index);
            archive->setChecked(archiveDraft[selectedMode]);
            if (selectedMode == Host)
                prepareHost();
            clearStatus();
            syncForm();
            fitHeight();
            if (selectedMode == Connect || (selectedMode == Host && share->isChecked()))
                emit dialog.refreshTailscaleRequested();
        });
        const auto update = [this] {
            clearStatus();
            syncForm();
        };
        QObject::connect(code, &QLineEdit::textChanged, &dialog, update);
        QObject::connect(manualTransport, &Segmented::currentChanged, &dialog, update);
        for (auto *field : {sshHost, manualPort, device, token})
            QObject::connect(field, &QLineEdit::textChanged, &dialog, update);
        QObject::connect(archive, &QAbstractButton::toggled, &dialog, [this] { clearStatus(); });
        QObject::connect(test, &QPushButton::clicked, &dialog, [this] {
            AppSettings result;
            if (!testing && !saving && readForm(result))
                beginTest(result, false);
        });
    }

    QWidget *makeHostDetails()
    {
        hostDetails = new QWidget(page);
        auto *column = new QVBoxLayout(hostDetails);
        column->setContentsMargins(0, 0, 0, 12);
        column->setSpacing(8);
        column->addWidget(paragraph(QStringLiteral("Screenshots are stored on this computer. "
                                                   "Keep it on with Frameline running to reach them from other devices."),
                                    "caption", hostDetails));
        hostStatus = new StatusLine(hostDetails);
        column->addWidget(hostStatus);
        auto *group = new Group(hostDetails);
        share = new Toggle(QStringLiteral("Access from other devices"));
        share->setObjectName(QStringLiteral("libraryShare"));
        share->setChecked(saved.hostedLibrary.share);
        group->addWidgetRow(share);
        column->addWidget(group);
        QObject::connect(share, &QAbstractButton::toggled, &dialog, [this](bool checked) {
            clearStatus();
            syncForm();
            fitHeight();
            if (checked)
                emit dialog.refreshTailscaleRequested();
        });
        return hostDetails;
    }

    QWidget *makeJoinDetails()
    {
        auto *group = new Group(page);
        code = new QLineEdit(libraryConnectionCode(saved.library));
        code->setObjectName(QStringLiteral("libraryInviteCode"));
        code->setPlaceholderText(QStringLiteral("Paste the code from the other computer"));
        code->setMaxLength(512);
        maskSecretField(*code);
        group->addRow(QStringLiteral("Connection code"), code);
        return joinDetails = group;
    }

    QWidget *makeTailscaleDetails()
    {
        tailscaleDetails = new QWidget(page);
        auto *column = new QVBoxLayout(tailscaleDetails);
        column->setContentsMargins(0, 8, 0, 14);
        column->setSpacing(8);
        column->addWidget(paragraph(QStringLiteral("Install Tailscale on both devices and sign in with the same account. "
                                                   "It connects them even when they’re on different networks."),
                                    "caption", tailscaleDetails));
        tailscaleStatus = new StatusLine(tailscaleDetails);
        column->addWidget(tailscaleStatus);
        auto *actions = new QHBoxLayout;
        downloadTailscale = new Button(Button::Ghost, Icon::None, QStringLiteral("Get Tailscale"), tailscaleDetails);
        auto *refresh = new Button(Button::Tonal, Icon::Refresh, QStringLiteral("Check Tailscale"), tailscaleDetails);
        copyCode = new Button(Button::Tonal, Icon::Copy, QStringLiteral("Copy connection code"), tailscaleDetails);
        for (auto *button : {copyCode, downloadTailscale, refresh}) {
            button->setAutoDefault(false);
            actions->addWidget(button);
        }
        actions->addStretch();
        column->addLayout(actions);
        QObject::connect(downloadTailscale, &QPushButton::clicked, &dialog, [this] {
            if (!QDesktopServices::openUrl(QUrl(QStringLiteral("https://tailscale.com/download"))))
                setStatus(QStringLiteral("Open tailscale.com/download in your browser to get Tailscale."),
                          StatusLine::Error);
        });
        QObject::connect(refresh, &QPushButton::clicked, &dialog, &SettingsWindow::refreshTailscaleRequested);
        QObject::connect(copyCode, &QPushButton::clicked, &dialog, [this] {
            const QString invite = hostedConnectionCode();
            if (invite.isEmpty())
                return;
            QGuiApplication::clipboard()->setText(invite);
            setStatus(QStringLiteral("Code copied. Paste it into Frameline on a device you trust."),
                      StatusLine::Success);
        });
        return tailscaleDetails;
    }

    QWidget *makeManualDetails()
    {
        manualDetails = new QWidget(page);
        auto *column = new QVBoxLayout(manualDetails);
        column->setContentsMargins(0, 0, 0, 12);
        column->setSpacing(10);
        column->addWidget(paragraph(QStringLiteral("Connect to a library server you’ve already set up."),
                                    "caption", manualDetails));
        manualTransport = new Segmented({QStringLiteral("This computer"), QStringLiteral("Over SSH")}, manualDetails);
        manualTransport->setObjectName(QStringLiteral("libraryManualTransport"));
        manualTransport->setAccessibleName(QStringLiteral("Manual connection type"));
        manualTransport->setCurrent(saved.library.transport == LibraryTransport::Ssh ? 1 : 0);
        column->addWidget(manualTransport);
        auto *fields = new Group(manualDetails);
        sshHost = new QLineEdit(saved.library.sshHost);
        sshHost->setObjectName(QStringLiteral("libraryHost"));
        sshHost->setPlaceholderText(QStringLiteral("screenshots-server"));
        sshHost->setMaxLength(128);
        sshHostRow = fields->addRow(QStringLiteral("SSH host"), sshHost);
        const bool manual = savedMode() == Advanced;
        manualPort = new QLineEdit(QString::number(manual ? saved.library.port : defaultLibraryPort));
        manualPort->setObjectName(QStringLiteral("libraryPort"));
        manualPort->setValidator(new QIntValidator(1, 65535, manualPort));
        manualPort->setPlaceholderText(QString::number(defaultLibraryPort));
        manualPort->setMaxLength(5);
        manualPort->setFixedWidth(120);
        auto *portField = new QWidget;
        auto *portLayout = new QHBoxLayout(portField);
        portLayout->setContentsMargins(0, 0, 0, 0);
        portLayout->addWidget(manualPort);
        portLayout->addStretch();
        fields->addRow(QStringLiteral("Port"), portField);
        token = new QLineEdit(manual ? saved.library.token : QString{});
        token->setObjectName(QStringLiteral("libraryToken"));
        token->setPlaceholderText(QStringLiteral("From the library server"));
        token->setMaxLength(256);
        maskSecretField(*token);
        fields->addRow(QStringLiteral("Access token"), token);
        column->addWidget(fields);
        auto *sshHint = paragraph(QStringLiteral("Use an SSH host or alias that already connects without prompting "
                                                 "and has a verified host key."), "caption", manualDetails);
        sshHint->setVisible(manualTransport->current() == 1);
        column->addWidget(sshHint);
        QObject::connect(manualTransport, &Segmented::currentChanged, &dialog, [this, sshHint](int index) {
            sshHint->setVisible(index == 1);
            fitHeight();
        });
        return manualDetails;
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
        cancel = new Button(Button::Tonal, Icon::None, QStringLiteral("Cancel"), footer);
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

    Mode savedMode() const
    {
        if (saved.hostedLibrary.enabled)
            return Host;
        if (saved.library.transport == LibraryTransport::Tailscale)
            return Connect;
        return saved.library == LibrarySettings{} ? Off : Advanced;
    }

    void prepareHost()
    {
        if (hostDraft.directory.isEmpty())
            hostDraft.directory = defaultHostedLibraryDirectory();
        if (hostDraft.token.isEmpty())
            hostDraft.token = generateLibraryToken();
    }

    AppSettings settings(QString *error = nullptr) const
    {
        if (error)
            error->clear();
        AppSettings result;
        result.autoCopyScreenshot = autoCopyScreenshot->isChecked();
        result.useSystemRegionShortcut = takeover->isChecked();
        result.hostedLibrary = saved.hostedLibrary;
        result.hostedLibrary.enabled = false;
        result.hostedLibrary.share = false;
        if (selectedMode == Off)
            return result;

        auto &library = result.library;
        const QString deviceName = device->text().trimmed();
        if (selectedMode == Host) {
            result.hostedLibrary = hostDraft;
            result.hostedLibrary.enabled = true;
            result.hostedLibrary.share = share->isChecked();
            library.port = result.hostedLibrary.port;
            library.token = result.hostedLibrary.token;
            library.device = deviceName.isEmpty() ? defaultLibraryDeviceName() : deviceName;
        } else if (selectedMode == Connect) {
            if (const auto parsed = librarySettingsFromConnectionCode(code->text(), deviceName, error))
                library = *parsed;
            else
                return result;
        } else {
            library.transport = manualTransport->current() == 1 ? LibraryTransport::Ssh : LibraryTransport::Local;
            if (library.transport == LibraryTransport::Ssh)
                library.sshHost = sshHost->text().trimmed();
            library.port = manualPort->text().isEmpty() ? defaultLibraryPort : manualPort->text().toInt();
            library.device = deviceName;
            library.token = token->text().trimmed();
        }
        library.autoArchive = archive->isChecked();
        if (error) {
            *error = result.hostedLibrary.validationError();
            if (error->isEmpty())
                *error = library.validationError();
            if (error->isEmpty() && !library.isConfigured())
                *error = QStringLiteral("Complete the library connection settings.");
        }
        return result;
    }

    QString hostedConnectionCode() const
    {
        if (selectedMode != Host || !hosting || !saved.hostedLibrary.enabled || !saved.hostedLibrary.share
            || !share->isChecked() || shareAddress.isEmpty() || tailscale.state != TailscaleState::Ready)
            return {};
        LibrarySettings library;
        library.transport = LibraryTransport::Tailscale;
        library.address = shareAddress;
        library.port = saved.hostedLibrary.port;
        library.token = saved.hostedLibrary.token;
        return libraryConnectionCode(library);
    }

    void setStatus(const QString &message, StatusLine::Severity severity = StatusLine::Neutral)
    {
        status->set(message, severity);
        if (!message.isEmpty())
            scroll->ensureWidgetVisible(status, 12, 12);
    }

    void clearStatus() { setStatus({}); }

    void syncForm()
    {
        const bool host = selectedMode == Host;
        const bool join = selectedMode == Connect;
        const bool manual = selectedMode == Advanced;
        const bool busy = testing || saving;
        hostOfflineNote->setVisible(saved.hostedLibrary.enabled && !host);
        libraryDetails->setVisible(selectedMode != Off);
        hostDetails->setVisible(host);
        joinDetails->setVisible(join);
        tailscaleDetails->setVisible(join || (host && share->isChecked()));
        manualDetails->setVisible(manual);
        sshHostRow->setVisible(manualTransport->current() == 1);
        test->setVisible(manual);
        connection->setEnabled(!busy);
        autostart->setEnabled(!busy && startupEditable);
        for (QWidget *control : {libraryDetails, hostDetails, joinDetails, tailscaleDetails, manualDetails,
                                 static_cast<QWidget *>(test), static_cast<QWidget *>(save),
                                 static_cast<QWidget *>(autoCopyScreenshot),
                                 static_cast<QWidget *>(takeover)}) {
            control->setEnabled(!busy);
        }

        const QString readyCode = hostedConnectionCode();
        copyCode->setVisible(!readyCode.isEmpty());
        downloadTailscale->setVisible(tailscale.state != TailscaleState::Ready);
        QString hostMessage;
        auto severity = StatusLine::Neutral;
        if (saved.hostedLibrary.enabled) {
            if (!hostError.isEmpty() && (!hosting || (share->isChecked() && hostError != tailscale.message))) {
                hostMessage = hostError;
                severity = StatusLine::Error;
            } else if (hosting) {
                hostMessage = readyCode.isEmpty() ? QStringLiteral("Library is ready on this computer.")
                                                   : QStringLiteral("Library is ready to share.");
                severity = StatusLine::Success;
            }
        }
        hostStatus->set(hostMessage, severity);
        tailscaleStatus->set(tailscale.message,
                             tailscale.state == TailscaleState::Ready ? StatusLine::Success : StatusLine::Neutral);

        QString action = QStringLiteral("Save");
        if (host && !saved.hostedLibrary.enabled) {
            action = QStringLiteral("Create library");
        } else if (host && !hosting) {
            action = QStringLiteral("Start library");
        } else if (host && share->isChecked() != saved.hostedLibrary.share) {
            action = QStringLiteral("Apply sharing");
        } else if (join && (savedMode() != Connect || code->text().trimmed() != libraryConnectionCode(saved.library))) {
            action = QStringLiteral("Connect");
        }
        if (save->text() != action) {
            save->setText(action);
            save->setAccessibleName(action);
            save->setFixedWidth(std::max(save->sizeHint().width(), 88));
        }
    }

    bool readForm(AppSettings &result)
    {
        QString error;
        result = settings(&error);
        if (error.isEmpty())
            return true;
        if (selectedMode == Connect) {
            code->setFocus();
        } else if (selectedMode == Advanced) {
            if (result.library.transport == LibraryTransport::Ssh && result.library.sshHost.isEmpty())
                sshHost->setFocus();
            else if (result.library.token.isEmpty())
                token->setFocus();
        }
        setStatus(error, StatusLine::Error);
        return false;
    }

    void beginTest(const AppSettings &result, bool connectOnSuccess)
    {
        testing = true;
        if (connectOnSuccess)
            pendingConnection = result;
        syncForm();
        setStatus(QStringLiteral("Testing connection…"));
        emit dialog.testRequested(result.library);
    }

    void submit()
    {
        if (testing || saving)
            return;
        AppSettings result;
        if (!readForm(result))
            return;
        if (selectedMode == Connect && result.library.storageKey() != saved.library.storageKey()) {
            beginTest(result, true);
            return;
        }
        requestSave(result);
    }

    void requestSave(const AppSettings &result)
    {
        closeAfterSave = selectedMode != Host || (saved.hostedLibrary.enabled && hosting
                                                  && result.hostedLibrary.share == saved.hostedLibrary.share);
        saving = true;
        clearStatus();
        syncForm();
        emit dialog.saveRequested(result, autostart->isChecked());
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

bool SettingsWindow::autostartEditable() const
{
    return ui->startupEditable;
}

void SettingsWindow::accept()
{
    ui->submit();
}

void SettingsWindow::setTestResult(const QString &error)
{
    if (!ui->testing) {
        return;
    }
    ui->testing = false;
    const auto pending = std::exchange(ui->pendingConnection, std::nullopt);
    ui->syncForm();
    if (!error.isEmpty()) {
        ui->setStatus(error, StatusLine::Error);
        return;
    }
    if (pending) {
        ui->requestSave(*pending);
    } else {
        ui->setStatus(QStringLiteral("Connected. This device can reach the library."), StatusLine::Success);
    }
}

void SettingsWindow::setSaveResult(const QString &error, const AppSettings &appliedSettings)
{
    if (!ui->saving)
        return;
    ui->saving = false;
    if (!error.isEmpty()) {
        ui->syncForm();
        ui->setStatus(error, StatusLine::Error);
        return;
    }
    ui->saved = appliedSettings;
    ui->hostDraft = appliedSettings.hostedLibrary;
    ui->syncForm();
    if (ui->closeAfterSave) {
        QDialog::accept();
    } else {
        ui->cancel->setText(QStringLiteral("Close"));
        ui->cancel->setAccessibleName(QStringLiteral("Close"));
        ui->fitHeight();
    }
}

void SettingsWindow::setTailscaleStatus(const TailscaleStatus &status)
{
    ui->tailscale = status;
    ui->syncForm();
}

void SettingsWindow::setHostedStatus(bool running, const QString &shareAddress, const QString &error)
{
    if (ui->hosting == running && ui->shareAddress == shareAddress && ui->hostError == error)
        return;
    ui->hosting = running;
    ui->shareAddress = shareAddress;
    ui->hostError = error;
    ui->syncForm();
    if (ui->selectedMode == Ui::Host)
        ui->fitHeight();
}
