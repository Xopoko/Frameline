#include "application.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QLocale>
#include <QTextStream>

#include <utility>

#if defined(Q_OS_WIN)
#  include <qt_windows.h>
#  include <shellapi.h>
#endif

namespace {

QStringList commandLine(int argc, char **argv)
{
#if defined(Q_OS_WIN)
    int count = 0;
    if (wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(), &count)) {
        QStringList result;
        for (int i = 0; i < count; ++i) {
            result.append(QString::fromWCharArray(arguments[i]));
        }
        LocalFree(arguments);
        return result;
    }
#endif
    QStringList result;
    for (int i = 0; i < argc; ++i) {
        result.append(QString::fromLocal8Bit(argv[i]));
    }
    return result;
}

int argumentError(const QString &message)
{
    QTextStream(stderr) << "screenframe: " << message << "\nUse --help for usage.\n";
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication::setApplicationName(QStringLiteral("ScreenFrame"));
    QCoreApplication::setOrganizationName(QStringLiteral("ScreenFrame"));
    QCoreApplication::setApplicationVersion(QStringLiteral(SCREENFRAME_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Capture and annotate screenshots.\n\n"
        "screenframe [--tray]\n"
        "screenframe capture full|region [--output <file.png>] [--no-preview]"));
    const auto help = parser.addHelpOption();
    const auto version = parser.addVersionOption();
    const QCommandLineOption tray(QStringLiteral("tray"), QStringLiteral("Start in the system tray."));
    const QCommandLineOption output({QStringLiteral("o"), QStringLiteral("output")},
                                   QStringLiteral("Save the capture as a PNG."), QStringLiteral("file.png"));
    const QCommandLineOption noPreview(QStringLiteral("no-preview"), QStringLiteral("Exit after saving the PNG."));
    parser.addOptions({tray, output, noPreview});

    if (!parser.parse(commandLine(argc, argv))) {
        return argumentError(parser.errorText());
    }
    if (parser.isSet(help)) {
        QTextStream(stdout) << parser.helpText();
        return 0;
    }
    if (parser.isSet(version)) {
        QTextStream(stdout) << QCoreApplication::applicationName() << ' '
                            << QCoreApplication::applicationVersion() << Qt::endl;
        return 0;
    }

    LaunchOptions options;
    const QStringList positional = parser.positionalArguments();
    if (!positional.isEmpty()) {
        if (positional.size() != 2 || positional[0] != QStringLiteral("capture")) {
            return argumentError(QStringLiteral("Expected 'capture full' or 'capture region'."));
        }
        if (positional[1] == QStringLiteral("full")) {
            options.mode = LaunchOptions::Mode::Full;
        } else if (positional[1] == QStringLiteral("region")) {
            options.mode = LaunchOptions::Mode::Region;
        } else {
            return argumentError(QStringLiteral("The capture mode must be 'full' or 'region'."));
        }
    }

    options.trayOnly = parser.isSet(tray);
    options.preview = !parser.isSet(noPreview);
    if (options.mode == LaunchOptions::Mode::Desktop) {
        if (parser.isSet(output) || !options.preview) {
            return argumentError(QStringLiteral("--output and --no-preview require the capture command."));
        }
    } else if (options.trayOnly) {
        return argumentError(QStringLiteral("Use --tray without a capture command."));
    }

    if (parser.isSet(output)) {
        options.output = parser.value(output);
        if (QFileInfo(options.output).suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0) {
            return argumentError(QStringLiteral("The --output path must end in .png."));
        }
    } else if (!options.preview) {
        return argumentError(QStringLiteral("--no-preview requires --output <file.png>."));
    }

    QLocale::setDefault(QLocale(QLocale::English));
    QApplication app(argc, argv);
    QApplication::setApplicationDisplayName(QStringLiteral("ScreenFrame"));
    QApplication::setDesktopFileName(QStringLiteral("io.github.xopoko.ScreenFrame"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/screenframe.png")));

    ScreenFrameApplication controller(std::move(options));
    controller.start();
    return app.exec();
}
