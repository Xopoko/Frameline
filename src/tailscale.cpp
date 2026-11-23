#include "tailscale.h"

#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUdpSocket>

#include <utility>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
constexpr int checkIntervalMs = 10'000;
constexpr int checkTimeoutMs = 5'000;
constexpr qsizetype maxOutputBytes = 4 * 1024 * 1024;

QString executablePath()
{
    QStringList candidates;
#ifdef Q_OS_WIN
    for (const char *key : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)"}) {
        const QString folder = qEnvironmentVariable(key);
        if (!folder.isEmpty())
            candidates.append(QDir(folder).filePath(QStringLiteral("Tailscale/tailscale.exe")));
    }
#elif defined(Q_OS_MACOS)
    candidates = {QStringLiteral("/Applications/Tailscale.app/Contents/MacOS/Tailscale"),
                  QStringLiteral("/usr/local/bin/tailscale"),
                  QStringLiteral("/opt/homebrew/bin/tailscale")};
#else
    candidates = {QStringLiteral("/usr/bin/tailscale"), QStringLiteral("/usr/local/bin/tailscale"),
                  QStringLiteral("/usr/sbin/tailscale"), QStringLiteral("/usr/local/sbin/tailscale")};
#endif
    for (const QString &path : candidates) {
        const QFileInfo file(path);
        if (file.isFile() && file.isExecutable())
            return file.absoluteFilePath();
    }
    return QStandardPaths::findExecutable(QStringLiteral("tailscale"));
}

bool isCanonicalAddress(const QString &address)
{
    return !address.isEmpty() && normalizedTailscaleAddress(address) == address;
}

QStringList activeAddresses()
{
    QStringList addresses;
    for (const auto &adapter : QNetworkInterface::allInterfaces()) {
        if (!adapter.flags().testFlag(QNetworkInterface::IsUp))
            continue;
        for (const auto &entry : adapter.addressEntries()) {
            if (isTailscaleAddress(entry.ip()))
                addresses.append(entry.ip().toString());
        }
    }
    return addresses;
}

TailscaleStatus stoppedStatus(QString message)
{
    TailscaleStatus status;
    status.state = TailscaleState::Stopped;
    status.message = std::move(message);
    return status;
}

TailscaleStatus unavailableStatus()
{
    return stoppedStatus(QStringLiteral("Could not check Tailscale. Open Tailscale, connect, and try again."));
}
}

bool isTailscaleAddress(const QHostAddress &address)
{
    static const QHostAddress ipv4Range(QStringLiteral("100.64.0.0"));
    static const QHostAddress ipv6Range(QStringLiteral("fd7a:115c:a1e0::"));
    return address.scopeId().isEmpty()
        && (address.isInSubnet(ipv4Range, 10) || address.isInSubnet(ipv6Range, 48));
}

QString normalizedTailscaleAddress(const QString &address)
{
    static const QRegularExpression literal(QStringLiteral("\\A[0-9A-Fa-f.:]{1,45}\\z"));
    if (!literal.match(address).hasMatch())
        return {};
    const QHostAddress parsed(address);
    if (!isTailscaleAddress(parsed))
        return {};
    const QString canonical = parsed.toString();
    // IPv4 has one accepted spelling. IPv6 can be written in several ways and is rewritten.
    if (parsed.protocol() == QAbstractSocket::IPv4Protocol && canonical != address)
        return {};
    return canonical;
}

bool TailscaleStatus::canReach(const QString &address) const
{
    return !sourceForPeer(address).isEmpty();
}

QString TailscaleStatus::sourceForPeer(const QString &address) const
{
    if (state != TailscaleState::Ready || validUntil.hasExpired() || !isCanonicalAddress(address)
        || (!localAddresses.contains(address) && !peerAddresses.contains(address)))
        return {};

    // UDP connect selects a route without transmitting a packet. Check it before an HTTP
    // request can send the library token over a competing route in the same address range.
    QUdpSocket route;
    route.setProxy(QNetworkProxy::NoProxy);
    route.connectToHost(QHostAddress(address), 9);
    const QString source = route.localAddress().toString();
    if (route.state() == QAbstractSocket::ConnectedState && localAddresses.contains(source)
        && activeAddresses().contains(source))
        return source;
    return {};
}

bool TailscaleStatus::operator==(const TailscaleStatus &other) const
{
    return state == other.state && localAddress == other.localAddress && localAddresses == other.localAddresses
        && peerAddresses == other.peerAddresses && message == other.message;
}

TailscaleMonitor::TailscaleMonitor(QObject *parent) : QObject(parent)
{
    status_.message = QStringLiteral("Checking Tailscale…");
    process_.setStandardErrorFile(QProcess::nullDevice());
#ifdef Q_OS_WIN
    process_.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        args->flags |= CREATE_NO_WINDOW;
    });
#elif defined(Q_OS_MACOS)
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("TAILSCALE_BE_CLI"), QStringLiteral("1"));
    process_.setProcessEnvironment(environment);
#endif

    refreshTimer_.setInterval(checkIntervalMs);
    refreshTimer_.setSingleShot(true);
    connect(&refreshTimer_, &QTimer::timeout, this, &TailscaleMonitor::refresh);
    timeout_.setInterval(checkTimeoutMs);
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        failed_ = true;
        process_.kill();
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &TailscaleMonitor::readOutput);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            publish(unavailableStatus());
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus exitStatus) {
        readOutput();
        if (failed_ || code != 0 || exitStatus != QProcess::NormalExit) {
            publish(unavailableStatus());
            return;
        }
        publish(parseStatus(output_, activeAddresses()));
    });
}

TailscaleMonitor::~TailscaleMonitor()
{
    process_.disconnect(this);
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished();
    }
}

TailscaleStatus TailscaleMonitor::status() const
{
    if (status_.state == TailscaleState::Ready && status_.validUntil.hasExpired())
        return unavailableStatus();
    return status_;
}

void TailscaleMonitor::refresh()
{
    if (process_.state() != QProcess::NotRunning)
        return;
    refreshTimer_.stop();
    const QString program = executablePath();
    if (program.isEmpty()) {
        TailscaleStatus status;
        status.state = TailscaleState::Missing;
        status.message = QStringLiteral("Install Tailscale to connect your devices over the internet.");
        publish(std::move(status));
        return;
    }

    failed_ = false;
    output_.clear();
    timeout_.start();
    process_.start(program, {QStringLiteral("status"), QStringLiteral("--json")}, QIODevice::ReadOnly);
}

void TailscaleMonitor::readOutput()
{
    const QByteArray bytes = process_.readAllStandardOutput();
    if (failed_)
        return;
    if (output_.size() + bytes.size() > maxOutputBytes) {
        failed_ = true;
        process_.kill();
        return;
    }
    output_ += bytes;
}

void TailscaleMonitor::publish(TailscaleStatus status)
{
    timeout_.stop();
    refreshTimer_.start();
    output_.clear();
    const bool changed = status_ != status;
    status_ = status;
    const QPointer<TailscaleMonitor> guard(this);
    if (changed)
        emit statusChanged(status);
    if (guard)
        emit refreshed(status);
}

TailscaleStatus TailscaleMonitor::parseStatus(const QByteArray &output, const QStringList &localAddresses)
{
    const auto object = QJsonDocument::fromJson(output).object();
    const QString backend = object.value(QStringLiteral("BackendState")).toString();
    if (backend == QStringLiteral("NeedsLogin"))
        return stoppedStatus(QStringLiteral("Open Tailscale and sign in on this device."));
    if (backend == QStringLiteral("NeedsMachineAuth"))
        return stoppedStatus(QStringLiteral("Approve this device in Tailscale, then try again."));
    if (backend.isEmpty())
        return unavailableStatus();
    if (backend != QStringLiteral("Running"))
        return stoppedStatus(QStringLiteral("Open Tailscale and connect on this device."));
    if (!object.value(QStringLiteral("TUN")).toBool())
        return stoppedStatus(QStringLiteral("Connect Tailscale using its system VPN on this device."));

    TailscaleStatus status;
    const auto self = object.value(QStringLiteral("Self")).toObject();
    for (const auto &value : self.value(QStringLiteral("TailscaleIPs")).toArray()) {
        const QString address = value.toString();
        if (isCanonicalAddress(address) && localAddresses.contains(address))
            status.localAddresses.append(address);
    }
    if (status.localAddresses.isEmpty())
        return stoppedStatus(QStringLiteral("Tailscale is connecting. Wait a moment and try again."));
    status.localAddresses.removeDuplicates();
    status.localAddresses.sort();
    // The canonical IPv4 strings sort before the IPv6 strings. Prefer IPv4 when available.
    status.localAddress = status.localAddresses.first();

    const auto peers = object.value(QStringLiteral("Peer")).toObject();
    for (const auto &value : peers) {
        const auto peer = value.toObject();
        if (!peer.value(QStringLiteral("InNetworkMap")).toBool()
            || peer.value(QStringLiteral("Expired")).toBool())
            continue;
        for (const auto &ip : peer.value(QStringLiteral("TailscaleIPs")).toArray()) {
            const QString address = ip.toString();
            if (isCanonicalAddress(address))
                status.peerAddresses.append(address);
        }
    }
    status.peerAddresses.removeDuplicates();
    status.peerAddresses.sort();
    status.state = TailscaleState::Ready;
    status.message = QStringLiteral("Tailscale is connected.");
    status.validUntil.setRemainingTime(checkIntervalMs + checkTimeoutMs);
    return status;
}
