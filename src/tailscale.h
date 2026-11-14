#pragma once

#include <QDeadlineTimer>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

enum class TailscaleState { Checking, Missing, Stopped, Ready };

struct TailscaleStatus {
    TailscaleState state = TailscaleState::Checking;
    QString localAddress;
    QStringList localAddresses;
    QStringList peerAddresses;
    QString message;

    bool canReach(const QString &address) const;
    QString sourceForPeer(const QString &address) const;
    bool operator==(const TailscaleStatus &other) const;

private:
    friend class TailscaleMonitor;
    QDeadlineTimer validUntil;
};

class TailscaleMonitor final : public QObject {
    Q_OBJECT

public:
    explicit TailscaleMonitor(QObject *parent = nullptr);
    ~TailscaleMonitor() override;

    TailscaleStatus status() const;
    void refresh();

signals:
    void statusChanged(const TailscaleStatus &status);
    void refreshed(const TailscaleStatus &status);

private:
    friend class TailscaleTests;
    static TailscaleStatus parseStatus(const QByteArray &output, const QStringList &localAddresses);
    void readOutput();
    void publish(TailscaleStatus status);

    QProcess process_;
    QTimer refreshTimer_;
    QTimer timeout_;
    QByteArray output_;
    bool failed_ = false;
    TailscaleStatus status_;
};
