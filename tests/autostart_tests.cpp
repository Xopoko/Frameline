#include "platform/autostart.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QXmlStreamReader>

#ifdef FREEZECUT_WINDOWS_STORE
#include <qt_windows.h>
#include <appmodel.h>
#endif

using namespace Qt::StringLiterals;

class AutostartTests final : public QObject {
    Q_OBJECT

private slots:
#ifdef FREEZECUT_WINDOWS_STORE
    void storeBuildNeedsPackageIdentityToRegister()
    {
        UINT32 length = 0;
        if (GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE)
            QSKIP("This test requires the unpackaged executable from the build directory.");

        QString error;
        QVERIFY(!Autostart::snapshot(&error).isEnabled());
        QVERIFY(!error.isEmpty());
        QVERIFY(!Autostart::setEnabled(true, &error));
        QVERIFY(!error.isEmpty());
    }
#endif

    void windowsCommandUsesOneQuotedExecutable()
    {
        QCOMPARE(Autostart::Detail::windowsCommand(u"C:/Apps/Screen Frame/FreezeCutGUI.exe"_s),
                 u"\"C:\\Apps\\Screen Frame\\FreezeCutGUI.exe\" --tray"_s);
        QVERIFY(Autostart::Detail::windowsCommand(u"C:/Apps/Screen\"Frame.exe"_s).isEmpty());
        QVERIFY(Autostart::Detail::windowsCommand(QString(261, u'a')).isEmpty());
    }

    void macAgentPreservesPathAndArguments()
    {
        const QString executable = u"/Applications/Screen & \"Frame\".app/Contents/MacOS/FreezeCut"_s;
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
        QCOMPARE(strings, (QStringList{u"io.github.xopoko.Frameline"_s, executable, u"--tray"_s}));
        QVERIFY(runAtLoad);
    }

    void linuxEntryEscapesDesktopSyntax()
    {
        const auto entry = Autostart::Detail::linuxEntry(u"/a b/%c/\"$`\\/freezecut"_s);
        const QByteArray escaped = QByteArray(2, '\\');
        const QByteArray expected = "Exec=\"/a b/%%c/" + escaped + '"' + escaped + '$'
            + escaped + '`' + QByteArray(4, '\\') + "/freezecut\" --tray\n";
        QVERIFY2(entry.contains(expected), entry.constData());
        QVERIFY(entry.contains("Name=FreezeCut\n"));
        QVERIFY(entry.contains("Terminal=false\n"));
        QVERIFY(Autostart::Detail::linuxEntry(u"/a\nExec=other/freezecut"_s).isEmpty());
    }

    void refreshRecognizesOnlyEarlierRegistrationsForThisInstall()
    {
        using Autostart::Detail::matchesPreviousEntry;
#ifdef Q_OS_WIN
        using Autostart::Detail::windowsCommand;
        const QString executable = u"C:/Apps/FreezeCut/bin/FreezeCutGUI.exe"_s;
        const auto frameline = windowsCommand(u"C:/Apps/Frameline/bin/FramelineGUI.exe"_s);
        const auto screenFrame = windowsCommand(u"C:/Apps/ScreenFrame/bin/ScreenFrameGUI.exe"_s);
        const auto otherInstall = windowsCommand(u"C:/Other/Frameline/bin/FramelineGUI.exe"_s);
        const auto manual = frameline + u" --custom"_s;
#elif defined(Q_OS_MACOS)
        using Autostart::Detail::macAgent;
        const QString executable = u"/Applications/FreezeCut.app/Contents/MacOS/FreezeCut"_s;
        const auto frameline = macAgent(u"/Applications/Frameline.app/Contents/MacOS/Frameline"_s);
        auto screenFrame = macAgent(u"/Applications/ScreenFrame.app/Contents/MacOS/ScreenFrame"_s);
        screenFrame.replace("io.github.xopoko.Frameline", "io.github.xopoko.ScreenFrame");
        const auto otherInstall = macAgent(u"/Other/Frameline.app/Contents/MacOS/Frameline"_s);
        const auto manual = frameline + "<!-- edited -->\n";
#else
        using Autostart::Detail::linuxEntry;
        const QString executable = u"/usr/bin/freezecut"_s;
        auto frameline = linuxEntry(u"/usr/bin/frameline"_s);
        frameline.replace("Name=FreezeCut", "Name=Frameline");
        auto screenFrame = linuxEntry(u"/usr/bin/screenframe"_s);
        screenFrame.replace("Name=FreezeCut", "Name=ScreenFrame");
        auto otherInstall = linuxEntry(u"/opt/bin/frameline"_s);
        otherInstall.replace("Name=FreezeCut", "Name=Frameline");
        const auto manual = frameline + "Hidden=true\n";
#endif
        QVERIFY(matchesPreviousEntry({frameline}, executable, false));
        QVERIFY(matchesPreviousEntry({screenFrame}, executable, true));
        QVERIFY(!matchesPreviousEntry({frameline}, executable, true));
        QVERIFY(!matchesPreviousEntry({manual}, executable, false));
        QVERIFY(!matchesPreviousEntry({otherInstall}, executable, false));
        QVERIFY(!matchesPreviousEntry({}, executable, false));
    }

    void fileRegistrationCanBeMovedAndRemoved()
    {
        const QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath(u"autostart/freezecut.desktop"_s);
        const auto first = Autostart::Detail::linuxEntry(u"/old/freezecut"_s);
        const auto second = Autostart::Detail::linuxEntry(u"/new/freezecut"_s);
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
