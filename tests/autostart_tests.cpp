#include "platform/autostart.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>

using namespace Qt::StringLiterals;

class AutostartTests final : public QObject {
    Q_OBJECT

private slots:
    void windowsCommandUsesOneQuotedExecutable()
    {
        QCOMPARE(Autostart::Detail::windowsCommand(u"C:/Apps/Screen Frame/ScreenFrameGUI.exe"_s),
                 u"\"C:\\Apps\\Screen Frame\\ScreenFrameGUI.exe\" --tray"_s);
        QVERIFY(Autostart::Detail::windowsCommand(u"C:/Apps/Screen\"Frame.exe"_s).isEmpty());
        QVERIFY(Autostart::Detail::windowsCommand(QString(261, u'a')).isEmpty());
    }

    void macAgentPreservesPathAndArguments()
    {
        const QString executable = u"/Applications/Screen & \"Frame\".app/Contents/MacOS/ScreenFrame"_s;
        const QByteArray contents = Autostart::Detail::macAgent(executable);
        QXmlStreamReader xml(contents);
        QStringList strings;
        QStringList keys;
        bool runAtLoad = false;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement())
                continue;
            if (xml.name() == u"key") {
                keys.append(xml.readElementText());
            } else if (xml.name() == u"string") {
                strings.append(xml.readElementText());
            } else if (xml.name() == u"true" && !keys.isEmpty() && keys.last() == u"RunAtLoad") {
                runAtLoad = true;
            }
        }
        QVERIFY2(!xml.hasError(), qPrintable(xml.errorString()));
        QCOMPARE(keys, (QStringList{u"Label"_s, u"ProgramArguments"_s, u"RunAtLoad"_s}));
        QCOMPARE(strings, (QStringList{u"io.github.xopoko.ScreenFrame"_s, executable, u"--tray"_s}));
        QVERIFY(runAtLoad);
    }

    void linuxEntryEscapesDesktopSyntax()
    {
        const auto entry = Autostart::Detail::linuxEntry(u"/a b/%c/\"$`\\/screenframe"_s);
        const QByteArray escaped = QByteArray(2, '\\');
        const QByteArray expected = "Exec=\"/a b/%%c/" + escaped + '"' + escaped + '$'
            + escaped + '`' + QByteArray(4, '\\') + "/screenframe\" --tray\n";
        QVERIFY2(entry.contains(expected), entry.constData());
        QVERIFY(entry.contains("Terminal=false\n"));
        QVERIFY(Autostart::Detail::linuxEntry(u"/a\nExec=other/screenframe"_s).isEmpty());
    }

    void fileRegistrationCanBeMovedAndRemoved()
    {
        const QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(u"autostart/screenframe.desktop"_s);
        const auto first = Autostart::Detail::linuxEntry(u"/old/screenframe"_s);
        const auto second = Autostart::Detail::linuxEntry(u"/new/screenframe"_s);
        QString error;
        const auto original = Autostart::Detail::fileSnapshot(path, &error);
        QVERIFY(!original.isEnabled());
        QVERIFY(error.isEmpty());
        QVERIFY2(Autostart::Detail::restoreFile(path, {first}, &error), qPrintable(error));
        const auto saved = Autostart::Detail::fileSnapshot(path, &error);
        QCOMPARE(saved.entry.toByteArray(), first);
        QVERIFY2(Autostart::Detail::restoreFile(path, {second}, &error), qPrintable(error));
        QCOMPARE(Autostart::Detail::fileSnapshot(path).entry.toByteArray(), second);
        QVERIFY2(Autostart::Detail::restoreFile(path, saved, &error), qPrintable(error));
        QCOMPARE(Autostart::Detail::fileSnapshot(path).entry.toByteArray(), first);
        QVERIFY2(Autostart::Detail::restoreFile(path, original, &error), qPrintable(error));
        QVERIFY2(Autostart::Detail::restoreFile(path, original, &error), qPrintable(error));
        QVERIFY(!QFile::exists(path));

        QVERIFY(QDir().mkpath(path));
        QVERIFY(!Autostart::Detail::restoreFile(path, {second}, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!Autostart::Detail::restoreFile(path, {}, &error));
        QVERIFY(!error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(AutostartTests)
#include "autostart_tests.moc"
