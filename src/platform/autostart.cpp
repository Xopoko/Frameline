#include "autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QXmlStreamWriter>

namespace Autostart {
namespace {

// Login registrations retain the original identifiers across the rename.
const auto appId = QStringLiteral("io.github.xopoko.Frameline");
const auto legacyAppId = QStringLiteral("io.github.xopoko.ScreenFrame");
const auto trayArgument = QStringLiteral("--tray");

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

#if !defined(FREEZECUT_MAC_APP_STORE) && !defined(FREEZECUT_WINDOWS_STORE)
QString executablePath()
{
    const QFileInfo current(QCoreApplication::applicationFilePath());
#ifdef Q_OS_WIN
    // The command-line executable would open a console when Windows starts it.
    const auto name = current.fileName().toCaseFolded();
    const QString canonical = current.dir().filePath(QStringLiteral("FreezeCutGUI.exe"));
    if (name == QStringLiteral("freezecut.exe"))
        return canonical;
    if (name == QStringLiteral("frameline.exe") || name == QStringLiteral("framelinegui.exe")) {
        return QFileInfo(canonical).isFile() ? canonical : current.dir().filePath(QStringLiteral("FramelineGUI.exe"));
    }
#endif
    return current.absoluteFilePath();
}

#ifdef Q_OS_WIN
const auto runKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const auto runValue = QStringLiteral("Frameline");
const auto legacyRunValue = QStringLiteral("ScreenFrame");
#else
QString entryPath(const QString &id = appId)
{
#ifdef Q_OS_MACOS
    return QDir::home().filePath(QStringLiteral("Library/LaunchAgents/") + id + QStringLiteral(".plist"));
#elif defined(FREEZECUT_LINUX_SNAP)
    const QString data = qEnvironmentVariable("SNAP_USER_DATA");
    if (!QDir::isAbsolutePath(data))
        return {};
    return QDir(data).filePath(QStringLiteral(".config/autostart/") + id + QStringLiteral(".desktop"));
#else
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (config.isEmpty())
        return {};
    return QDir(config).filePath(QStringLiteral("autostart/") + id + QStringLiteral(".desktop"));
#endif
}

QByteArray entryContents(const QString &executable)
{
#ifdef Q_OS_MACOS
    return Detail::macAgent(executable);
#else
    return Detail::linuxEntry(executable);
#endif
}
#endif
#endif

} // namespace

QString Detail::windowsCommand(const QString &executable)
{
    if (executable.isEmpty() || executable.contains(u'"') || executable.contains(u'\0')
        || executable.contains(u'\r') || executable.contains(u'\n')) {
        return {};
    }
    QString nativePath = executable;
    nativePath.replace(u'/', u'\\');
    const QString command = u'"' + nativePath + QStringLiteral("\" ") + trayArgument;
    // Windows documents a 260-character limit for Run commands.
    return command.size() <= 260 ? command : QString();
}

namespace {

QByteArray macAgentWithLabel(const QString &executable, const QString &label)
{
    if (executable.isEmpty())
        return {};
    QByteArray contents;
    QXmlStreamWriter xml(&contents);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeDTD(QStringLiteral("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                                "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"));
    xml.writeStartElement(QStringLiteral("plist"));
    xml.writeAttribute(QStringLiteral("version"), QStringLiteral("1.0"));
    xml.writeStartElement(QStringLiteral("dict"));
    xml.writeTextElement(QStringLiteral("key"), QStringLiteral("Label"));
    xml.writeTextElement(QStringLiteral("string"), label);
    xml.writeTextElement(QStringLiteral("key"), QStringLiteral("ProgramArguments"));
    xml.writeStartElement(QStringLiteral("array"));
    xml.writeTextElement(QStringLiteral("string"), executable);
    xml.writeTextElement(QStringLiteral("string"), trayArgument);
    xml.writeEndElement();
    xml.writeTextElement(QStringLiteral("key"), QStringLiteral("RunAtLoad"));
    xml.writeEmptyElement(QStringLiteral("true"));
    xml.writeEndDocument();
    return xml.hasError() ? QByteArray() : contents;
}

QByteArray linuxEntryWithName(const QString &executable, const QByteArray &name)
{
    if (executable.isEmpty() || executable.contains(u'=') || executable.contains(u'\0')
        || executable.contains(u'\r') || executable.contains(u'\n')) {
        return {};
    }
    QString argument;
    for (const QChar character : executable) {
        // A desktop entry's string escapes are decoded before Exec quoting.
        if (character == u'\\') {
            argument += QString(4, u'\\');
            continue;
        }
        if (character == u'"' || character == u'`' || character == u'$')
            argument += QString(2, u'\\');
        else if (character == u'%')
            argument += u'%';
        argument += character;
    }
    return "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=\""
        + argument.toUtf8() + "\" " + trayArgument.toLatin1() + "\nTerminal=false\n";
}

} // namespace

QByteArray Detail::macAgent(const QString &executable)
{
    return macAgentWithLabel(executable, appId);
}

QByteArray Detail::linuxEntry(const QString &executable)
{
    return linuxEntryWithName(executable, "FreezeCut");
}

bool Detail::matchesPreviousEntry(const State &state, const QString &executable, bool screenFrame)
{
    const QFileInfo current(executable);
    const QString previousName = screenFrame ? QStringLiteral("ScreenFrame") : QStringLiteral("Frameline");
#ifdef Q_OS_WIN
    if (!state.isEnabled() || current.fileName().compare(QStringLiteral("FreezeCutGUI.exe"), Qt::CaseInsensitive) != 0)
        return false;
    const QString previousExe = previousName + QStringLiteral("GUI.exe");
    const QString command = state.entry.toString();
    if (command.isEmpty())
        return false;
    if (command.compare(windowsCommand(current.dir().filePath(previousExe)), Qt::CaseInsensitive) == 0)
        return true;
    // The Windows ZIP extracts to a product-named folder with the executables in bin.
    const QString suffix = QStringLiteral("/FreezeCut/bin");
    const QString directory = current.absolutePath();
    return directory.endsWith(suffix, Qt::CaseInsensitive)
        && command.compare(windowsCommand(directory.chopped(suffix.size()) + u'/' + previousName
                            + QStringLiteral("/bin/") + previousExe), Qt::CaseInsensitive) == 0;
#elif defined(Q_OS_MACOS)
    if (!state.isEnabled() || current.fileName() != QStringLiteral("FreezeCut"))
        return false;
    const QByteArray entry = state.entry.toByteArray();
    const QString &label = screenFrame ? legacyAppId : appId;
    if (entry == macAgentWithLabel(current.dir().filePath(previousName), label))
        return true;
    const QString suffix = QStringLiteral("/FreezeCut.app/Contents/MacOS");
    const QString directory = current.absolutePath();
    return directory.endsWith(suffix)
        && entry == macAgentWithLabel(directory.chopped(suffix.size()) + u'/' + previousName
                                     + QStringLiteral(".app/Contents/MacOS/") + previousName, label);
#else
    return state.isEnabled() && current.fileName() == QStringLiteral("freezecut")
        && state.entry.toByteArray() == linuxEntryWithName(current.dir().filePath(previousName.toLower()),
                                                          previousName.toLatin1());
#endif
}

State Detail::fileSnapshot(const QString &path, QString *error)
{
    if (error)
        error->clear();
    const QFileInfo entry(path);
    if (!entry.exists() && !entry.isSymLink())
        return {};
    if (entry.isSymLink() || !entry.isFile()) {
        fail(error, QStringLiteral("The FreezeCut startup entry is not a regular file."));
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("Could not read the FreezeCut startup entry. %1").arg(file.errorString()));
        return {};
    }
    constexpr qint64 maxEntrySize = 64 * 1024;
    const QByteArray contents = file.read(maxEntrySize + 1);
    if (file.error() != QFileDevice::NoError || contents.size() > maxEntrySize) {
        fail(error, QStringLiteral("Could not read the FreezeCut startup entry."));
        return {};
    }
    return {contents};
}

bool Detail::restoreFile(const QString &path, const State &state, QString *error)
{
    if (error)
        error->clear();
    const QFileInfo entry(path);
    if (!state.isEnabled()) {
        QFile file(path);
        if (file.remove() || (!entry.exists() && !entry.isSymLink()))
            return true;
        return fail(error, QStringLiteral("Could not remove the FreezeCut startup entry. %1").arg(file.errorString()));
    }
    if (entry.isSymLink() || (entry.exists() && !entry.isFile()))
        return fail(error, QStringLiteral("Could not create the FreezeCut startup entry."));
    if (!QDir().mkpath(entry.absolutePath()))
        return fail(error, QStringLiteral("Could not create the startup folder."));

    const QByteArray contents = state.entry.toByteArray();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit())
        return fail(error, QStringLiteral("Could not save the FreezeCut startup entry. %1").arg(file.errorString()));
    return true;
}

bool isEnabled(QString *error)
{
    return snapshot(error).isEnabled();
}

#if !defined(FREEZECUT_MAC_APP_STORE) && !defined(FREEZECUT_WINDOWS_STORE)
State snapshot(QString *error)
{
    if (error)
        error->clear();
#ifdef Q_OS_WIN
    QSettings run(runKey, QSettings::NativeFormat);
    const QVariant registered = run.value(runValue);
    if (run.status() != QSettings::NoError) {
        fail(error, QStringLiteral("Could not read the Windows startup setting."));
        return {};
    }
    return {registered};
#else
    const QString path = entryPath();
    if (path.isEmpty()) {
        fail(error, QStringLiteral("Could not find the startup folder."));
        return {};
    }
    return Detail::fileSnapshot(path, error);
#endif
}

bool restore(const State &state, QString *error)
{
    if (error)
        error->clear();
#ifdef Q_OS_WIN
    QSettings run(runKey, QSettings::NativeFormat);
    if (state.isEnabled())
        run.setValue(runValue, state.entry);
    else
        run.remove(runValue);
    run.sync();
    if (run.status() != QSettings::NoError)
        return fail(error, QStringLiteral("Could not save the Windows startup setting."));
    return true;
#else
    const QString path = entryPath();
    if (path.isEmpty())
        return fail(error, QStringLiteral("Could not find the startup folder."));
    return Detail::restoreFile(path, state, error);
#endif
}

namespace {

State startupEntry(const QString &executable, QString *error)
{
    const QFileInfo executableFile(executable);
    if (!executableFile.isFile() || !executableFile.isExecutable()) {
        fail(error, QStringLiteral("Could not find the FreezeCut application to start at login."));
        return {};
    }
#ifdef Q_OS_WIN
    const QString command = Detail::windowsCommand(executable);
    if (command.isEmpty()) {
        fail(error, QStringLiteral("Move FreezeCut to a shorter folder path and try again."));
        return {};
    }
    return {command};
#else
    const QByteArray contents = entryContents(executable);
    if (contents.isEmpty()) {
        fail(error, QStringLiteral("Move FreezeCut to a folder with a simpler name and try again."));
        return {};
    }
    return {contents};
#endif
}

} // namespace

bool setEnabled(bool enabled, QString *error)
{
    if (!enabled)
        return restore({}, error);
    const State entry = startupEntry(executablePath(), error);
    return entry.isEnabled() && restore(entry, error);
}

void refreshAtStartup()
{
    QString error;
    const State current = snapshot(&error);
    if (!error.isEmpty())
        return;
    const QString executable = executablePath();
    const State desired = startupEntry(executable, nullptr);
    if (!desired.isEnabled())
        return;
    const bool refreshCurrent = Detail::matchesPreviousEntry(current, executable, false);
    if (current.isEnabled() && !refreshCurrent && current.entry != desired.entry)
        return;

#ifdef Q_OS_WIN
    QSettings run(runKey, QSettings::NativeFormat);
    const State legacy{run.value(legacyRunValue)};
#else
    const QString legacyPath = entryPath(legacyAppId);
    if (legacyPath.isEmpty())
        return;
    const State legacy = Detail::fileSnapshot(legacyPath);
#endif
    const bool removeLegacy = Detail::matchesPreviousEntry(legacy, executable, true);
    if ((refreshCurrent || (!current.isEnabled() && removeLegacy)) && !restore(desired))
        return;
    if (!removeLegacy)
        return;
#ifdef Q_OS_WIN
    run.remove(legacyRunValue);
    run.sync();
#else
    Detail::restoreFile(legacyPath, {});
#endif
}
#endif

} // namespace Autostart
