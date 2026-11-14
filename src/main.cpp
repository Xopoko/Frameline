#include "application.h"
#include "singleinstance.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QLocale>
#include <QMessageBox>
#include <QTextStream>

#include <memory>
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
    QTextStream(stderr) << "frameline: " << message << "\nUse --help for usage.\n";
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication::setApplicationName(QStringLiteral("Frameline"));
    QCoreApplication::setOrganizationName(QStringLiteral("Frameline"));
    QCoreApplication::setApplicationVersion(QStringLiteral(FRAMELINE_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Capture and annotate screenshots.\n\n"
        "frameline [--tray]\n"
        "frameline library\n"
        "frameline settings\n"
        "frameline capture full|region [--output <file.png>] [--no-preview]"));
    const auto help = parser.addHelpOption();
    const auto version = parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("Open the library, settings or capture the screen."),
                                 QStringLiteral("[library | settings | capture full | capture region]"));
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
    if (positional == QStringList{QStringLiteral("library")}) {
        options.mode = LaunchOptions::Mode::Library;
    } else if (positional == QStringList{QStringLiteral("settings")}) {
        options.mode = LaunchOptions::Mode::Settings;
    } else if (!positional.isEmpty()) {
        if (positional.size() != 2 || positional[0] != QStringLiteral("capture")) {
            return argumentError(QStringLiteral("Expected 'library', 'settings', 'capture full' or 'capture region'."));
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
    const bool capture = options.mode == LaunchOptions::Mode::Full || options.mode == LaunchOptions::Mode::Region;
    if (!capture && (parser.isSet(output) || !options.preview)) {
        return argumentError(QStringLiteral("--output and --no-preview require the capture command."));
    }
    if (options.trayOnly && options.mode != LaunchOptions::Mode::Desktop) {
        return argumentError(QStringLiteral("Use --tray without a command."));
    }

    if (parser.isSet(output)) {
        options.output = parser.value(output);
        if (QFileInfo(options.output).suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0) {
            return argumentError(QStringLiteral("The --output path must end in .png."));
        }
    } else if (!options.preview) {
        return argumentError(QStringLiteral("--no-preview requires --output <file.png>."));
    }

#if defined(Q_OS_MACOS)
    if (options.trayOnly)
        qputenv("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM", "1");
#endif
    QLocale::setDefault(QLocale(QLocale::English));
    QApplication app(argc, argv);
    QApplication::setApplicationDisplayName(QStringLiteral("Frameline"));
    QApplication::setDesktopFileName(QStringLiteral("io.github.xopoko.Frameline"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/frameline.png")));

    std::unique_ptr<SingleInstance> instance;
    if (!capture) {
        const auto request = options.trayOnly ? SingleInstance::Request::Quiet
            : options.mode == LaunchOptions::Mode::Library ? SingleInstance::Request::Library
            : options.mode == LaunchOptions::Mode::Settings ? SingleInstance::Request::Settings
            : SingleInstance::Request::Editor;
        instance = std::make_unique<SingleInstance>();
        QString error;
        const auto result = instance->start(request, &error);
        if (result == SingleInstance::Result::Forwarded)
            return 0;
        if (result == SingleInstance::Result::Failed) {
            QTextStream(stderr) << error << Qt::endl;
            if (!options.trayOnly)
                QMessageBox::critical(nullptr, QApplication::applicationName(), error);
            return 1;
        }
    }

    FramelineApplication controller(std::move(options));
    if (instance) {
        QObject::connect(instance.get(), &SingleInstance::activationRequested, &controller,
                         [&controller](SingleInstance::Request request) {
            switch (request) {
            case SingleInstance::Request::Editor: controller.activate(LaunchOptions::Mode::Desktop); break;
            case SingleInstance::Request::Library: controller.activate(LaunchOptions::Mode::Library); break;
            case SingleInstance::Request::Settings: controller.activate(LaunchOptions::Mode::Settings); break;
            case SingleInstance::Request::Quiet: break;
            }
        });
#if defined(Q_OS_MACOS)
        // Finder and the Dock reopen an existing app by activating it instead of starting a process.
        QObject::connect(&app, &QGuiApplication::applicationStateChanged, &controller,
                         [&controller](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive)
                return;
            for (const QWidget *window : QApplication::topLevelWidgets()) {
                if (window->isVisible())
                    return;
            }
            controller.activate(LaunchOptions::Mode::Desktop);
        });
#endif
    }
    controller.start();
    return app.exec();
}
