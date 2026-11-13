#include "tailscale.h"

#include <QtTest>

class TailscaleTests final : public QObject {
    Q_OBJECT

private slots:
    void acceptsOnlyActiveLocalAddress();
    void rejectsUnavailableTunnel_data();
    void rejectsUnavailableTunnel();
};

void TailscaleTests::acceptsOnlyActiveLocalAddress()
{
    const QByteArray output = R"({
        "BackendState": "Running",
        "TUN": true,
        "Self": {"TailscaleIPs": ["100.64.12.1", "100.64.12.2", "fd7a:115c:a1e0::2"]},
        "Peer": {
            "nodekey:1": {"InNetworkMap": true, "TailscaleIPs": ["100.64.12.3", "fd7a:115c:a1e0::3", "192.168.0.10"]},
            "nodekey:2": {"InNetworkMap": false, "TailscaleIPs": ["100.64.12.4"]},
            "nodekey:3": {"InNetworkMap": true, "Expired": true, "TailscaleIPs": ["100.64.12.5"]}
        }
    })";
    const QStringList localAddresses{QStringLiteral("100.64.12.2"), QStringLiteral("fd7a:115c:a1e0::2")};
    const auto status = TailscaleMonitor::parseStatus(output, localAddresses);
    QCOMPARE(status.state, TailscaleState::Ready);
    QCOMPARE(status.localAddress, QStringLiteral("100.64.12.2"));
    QCOMPARE(status.localAddresses, localAddresses);
    const QStringList peers{QStringLiteral("100.64.12.3"), QStringLiteral("fd7a:115c:a1e0::3")};
    QCOMPARE(status.peerAddresses, peers);
    QVERIFY(!status.canReach(QStringLiteral("192.168.0.10")));
    QVERIFY(!status.canReach(QStringLiteral("100.64.12.4")));
    QVERIFY(!status.canReach(QStringLiteral("100.64.12.5")));
}

void TailscaleTests::rejectsUnavailableTunnel_data()
{
    QTest::addColumn<QByteArray>("output");
    QTest::newRow("stopped") << QByteArray(R"({"BackendState":"Stopped","TUN":true,"Self":{"TailscaleIPs":["100.64.12.2"]}})");
    QTest::newRow("userspace") << QByteArray(R"({"BackendState":"Running","TUN":false,"Self":{"TailscaleIPs":["100.64.12.2"]}})");
    QTest::newRow("other-interface") << QByteArray(R"({"BackendState":"Running","TUN":true,"Self":{"TailscaleIPs":["100.64.12.3"]}})");
    QTest::newRow("malformed") << QByteArray("{invalid json");
}

void TailscaleTests::rejectsUnavailableTunnel()
{
    QFETCH(QByteArray, output);
    const auto status = TailscaleMonitor::parseStatus(output, {QStringLiteral("100.64.12.2")});
    QCOMPARE(status.state, TailscaleState::Stopped);
    QVERIFY(status.localAddress.isEmpty());
    QVERIFY(status.peerAddresses.isEmpty());
    QVERIFY(!status.canReach(QStringLiteral("100.64.12.2")));
}

QTEST_GUILESS_MAIN(TailscaleTests)
#include "tailscale_tests.moc"
