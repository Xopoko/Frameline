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

const auto appId = QStringLiteral("io.github.xopoko.Frameline");
const auto trayArgument = QStringLiteral("--tray");

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

QString executablePath()
{
    const QFileInfo current(QCoreApplication::applicationFilePath());
#ifdef Q_OS_WIN
    // The command-line executable would open a console when Windows starts it.
    if (current.fileName().compare(QStringLiteral("frameline.exe"), Qt::CaseInsensitive) == 0)
        return current.dir().filePath(QStringLiteral("FramelineGUI.exe"));
#endif
    return current.absoluteFilePath();
}

// Frameline was called ScreenFrame, and its startup entry used that name.
const auto legacyAppId = QStringLiteral("io.github.xopoko.ScreenFrame");

#ifdef Q_OS_WIN
const auto runKey = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const auto runValue = QStringLiteral("Frameline");
const auto legacyRunValue = QStringLiteral("ScreenFrame");
#else
QString entryPath(const QString &id = appId)
{
#ifdef Q_OS_MACOS
    return QDir::home().filePath(QStringLiteral("Library/LaunchAgents/") + id + QStringLiteral(".plist"));
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

QByteArray Detail::macAgent(const QString &executable)
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
    xml.writeTextElement(QStringLiteral("string"), appId);
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

QByteArray Detail::linuxEntry(const QString &executable)
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
    return "[Desktop Entry]\nType=Application\nName=Frameline\nExec=\""
        + argument.toUtf8() + "\" " + trayArgument.toLatin1() + "\nTerminal=false\n";
}

State Detail::fileSnapshot(const QString &path, QString *error)
{
    if (error)
        error->clear();
    const QFileInfo entry(path);
    if (!entry.exists() && !entry.isSymLink())
        return {};
    if (entry.isSymLink() || !entry.isFile()) {
        fail(error, QStringLiteral("The Frameline startup entry is not a regular file."));
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("Could not read the Frameline startup entry. %1").arg(file.errorString()));
        return {};
    }
    constexpr qint64 maxEntrySize = 64 * 1024;
    const QByteArray contents = file.read(maxEntrySize + 1);
    if (file.error() != QFileDevice::NoError || contents.size() > maxEntrySize) {
        fail(error, QStringLiteral("Could not read the Frameline startup entry."));
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
        return fail(error, QStringLiteral("Could not remove the Frameline startup entry. %1").arg(file.errorString()));
    }
    if (entry.isSymLink() || (entry.exists() && !entry.isFile()))
        return fail(error, QStringLiteral("Could not create the Frameline startup entry."));
    if (!QDir().mkpath(entry.absolutePath()))
        return fail(error, QStringLiteral("Could not create the startup folder."));

    const QByteArray contents = state.entry.toByteArray();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit())
        return fail(error, QStringLiteral("Could not save the Frameline startup entry. %1").arg(file.errorString()));
    return true;
}

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

bool isEnabled(QString *error)
{
    return snapshot(error).isEnabled();
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

bool setEnabled(bool enabled, QString *error)
{
    if (!enabled)
        return restore({}, error);

    const QString executable = executablePath();
    const QFileInfo executableFile(executable);
    if (!executableFile.isFile() || !executableFile.isExecutable())
        return fail(error, QStringLiteral("Could not find the Frameline application to start at login."));
#ifdef Q_OS_WIN
    const QString command = Detail::windowsCommand(executable);
    if (command.isEmpty())
        return fail(error, QStringLiteral("Move Frameline to a shorter folder path and try again."));
    return restore({command}, error);
#else
    const QByteArray contents = entryContents(executable);
    if (contents.isEmpty())
        return fail(error, QStringLiteral("Move Frameline to a folder with a simpler name and try again."));
    return restore({contents}, error);
#endif
}

void moveFromScreenFrame()
{
#ifdef Q_OS_WIN
    QSettings run(runKey, QSettings::NativeFormat);
    if (!run.contains(legacyRunValue))
        return;
    run.remove(legacyRunValue);
    run.sync();
#else
    const QString legacy = entryPath(legacyAppId);
    if (legacy.isEmpty() || !QFileInfo(legacy).isFile() || !QFile::remove(legacy))
        return;
#endif
    // The old entry pointed at the ScreenFrame executable. Start this one at login instead.
    setEnabled(true);
}

} // namespace Autostart
