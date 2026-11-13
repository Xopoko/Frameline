#include "application.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
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
    QTextStream(stderr) << "screenframe: " << message << "\n" << "Справка доступна через --help.\n";
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
        "Захват экрана и редактирование скриншотов.\n\n"
        "screenframe [--tray]\n"
        "screenframe capture full|region [--output <file.png>] [--no-preview]"));
    const auto help = parser.addHelpOption();
    const auto version = parser.addVersionOption();
    const QCommandLineOption tray(QStringLiteral("tray"), QStringLiteral("Запустить приложение в системном трее."));
    const QCommandLineOption output({QStringLiteral("o"), QStringLiteral("output")},
                                   QStringLiteral("Сразу сохранить снимок в PNG."), QStringLiteral("file.png"));
    const QCommandLineOption noPreview(QStringLiteral("no-preview"), QStringLiteral("Завершиться после записи PNG."));
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
            return argumentError(QStringLiteral("Ожидается команда capture full или capture region."));
        }
        if (positional[1] == QStringLiteral("full")) {
            options.mode = LaunchOptions::Mode::Full;
        } else if (positional[1] == QStringLiteral("region")) {
            options.mode = LaunchOptions::Mode::Region;
        } else {
            return argumentError(QStringLiteral("Режим захвата должен быть full или region."));
        }
    }

    options.trayOnly = parser.isSet(tray);
    options.preview = !parser.isSet(noPreview);
    if (options.mode == LaunchOptions::Mode::Desktop) {
        if (parser.isSet(output) || !options.preview) {
            return argumentError(QStringLiteral("Параметры --output и --no-preview относятся к команде capture."));
        }
    } else if (options.trayOnly) {
        return argumentError(QStringLiteral("Параметр --tray запускает приложение без команды capture."));
    }

    if (parser.isSet(output)) {
        options.output = parser.value(output);
        if (QFileInfo(options.output).suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) != 0) {
            return argumentError(QStringLiteral("Укажите путь с расширением .png для --output."));
        }
    } else if (!options.preview) {
        return argumentError(QStringLiteral("Для --no-preview укажите --output <file.png>."));
    }

    QApplication app(argc, argv);
    QApplication::setApplicationDisplayName(QStringLiteral("ScreenFrame"));
    QApplication::setDesktopFileName(QStringLiteral("io.github.xopoko.ScreenFrame"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/screenframe.png")));

    ScreenFrameApplication controller(std::move(options));
    controller.start();
    return app.exec();
}
