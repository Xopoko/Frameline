#include "singleinstance.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <future>

class SingleInstanceTests final : public QObject {
    Q_OBJECT

private slots:
    void forwardsOnlyVisibleLaunches();
    void reopensAfterPrimaryExits();
};

void SingleInstanceTests::forwardsOnlyVisibleLaunches()
{
    const QTemporaryDir directory;
    QVERIFY(directory.isValid());
    SingleInstance primary(directory.path());
    QCOMPARE(primary.start(SingleInstance::Request::Quiet), SingleInstance::Result::Primary);
    QSignalSpy requested(&primary, &SingleInstance::activationRequested);

    for (const auto request : {SingleInstance::Request::Quiet, SingleInstance::Request::Editor,
                               SingleInstance::Request::Library, SingleInstance::Request::Settings}) {
        auto forwarded = std::async(std::launch::async, [path = directory.path(), request] {
            SingleInstance secondary(path);
            return secondary.start(request);
        });
        QVERIFY(QTest::qWaitFor([&] {
            return forwarded.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
        }, 6000));
        QCOMPARE(forwarded.get(), SingleInstance::Result::Forwarded);
    }

    QCOMPARE(requested.size(), 3);
    QCOMPARE(requested.at(0).at(0).value<SingleInstance::Request>(), SingleInstance::Request::Editor);
    QCOMPARE(requested.at(1).at(0).value<SingleInstance::Request>(), SingleInstance::Request::Library);
    QCOMPARE(requested.at(2).at(0).value<SingleInstance::Request>(), SingleInstance::Request::Settings);
}

void SingleInstanceTests::reopensAfterPrimaryExits()
{
    const QTemporaryDir directory;
    QVERIFY(directory.isValid());
    {
        SingleInstance primary(directory.path());
        QCOMPARE(primary.start(SingleInstance::Request::Editor), SingleInstance::Result::Primary);
    }

    SingleInstance next(directory.path());
    QCOMPARE(next.start(SingleInstance::Request::Library), SingleInstance::Result::Primary);
}

QTEST_GUILESS_MAIN(SingleInstanceTests)
#include "singleinstance_tests.moc"
