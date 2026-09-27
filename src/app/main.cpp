// PulseScope entry point.
#ifndef __linux__
#error "PulseScope reads Linux /proc and /sys interfaces and only supports Linux."
#endif

#include "app/appicon.h"
#include "app/cli.h"
#include "app/mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QPixmap>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <optional>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace {

int g_signalPipe[2] = {-1, -1};

void onTerminationSignal(int)
{
    const int saved = errno;
    const char byte = 1;
    [[maybe_unused]] const ssize_t n = ::write(g_signalPipe[1], &byte, 1);
    errno = saved;
}

// SIGINT/SIGTERM close the window normally (saving settings and stopping the
// collector thread) instead of killing the process mid-sample.
void installSignalHandlers(QWidget* window)
{
    if (::pipe2(g_signalPipe, O_CLOEXEC | O_NONBLOCK) != 0)
        return;
    auto* notifier = new QSocketNotifier(g_signalPipe[0], QSocketNotifier::Read, window);
    QObject::connect(notifier, &QSocketNotifier::activated, window, [window] {
        char buf[16];
        while (::read(g_signalPipe[0], buf, sizeof buf) > 0) {
        }
        window->close();
    });
    struct sigaction sa {};
    sa.sa_handler = onTerminationSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PulseScope"));
    QApplication::setOrganizationName(QStringLiteral("PulseScope"));
    QApplication::setApplicationVersion(QStringLiteral(PULSESCOPE_VERSION));
    QApplication::setDesktopFileName(QStringLiteral(PULSESCOPE_APP_ID));
    QApplication::setWindowIcon(pulseScopeIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Native Linux performance monitor"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption intervalOpt(QStringLiteral("interval"),
                                         QStringLiteral("Sampling interval in seconds: 0.5, 1, 2 or 5."),
                                         QStringLiteral("seconds"));
    const QCommandLineOption pageOpt(QStringLiteral("page"),
                                     QStringLiteral("Start on a page: cpu, memory, disks, network, temperatures "
                                                    "or processes."),
                                     QStringLiteral("name"));
    const QCommandLineOption screenshotOpt(QStringLiteral("screenshot"),
                                           QStringLiteral("Save a PNG of the window after --screenshot-delay "
                                                          "seconds (for testing)."),
                                           QStringLiteral("file"));
    const QCommandLineOption delayOpt(QStringLiteral("screenshot-delay"),
                                      QStringLiteral("Seconds before the screenshot (default 5)."),
                                      QStringLiteral("seconds"), QStringLiteral("5"));
    const QCommandLineOption quitOpt(QStringLiteral("quit-after"),
                                     QStringLiteral("Close the window after this many seconds (for testing)."),
                                     QStringLiteral("seconds"));
    const QCommandLineOption sizeOpt(QStringLiteral("size"),
                                     QStringLiteral("Initial window size in pixels, e.g. 1180x780 (for testing and "
                                                    "screenshots)."),
                                     QStringLiteral("WIDTHxHEIGHT"));
    parser.addOption(intervalOpt);
    parser.addOption(pageOpt);
    parser.addOption(screenshotOpt);
    parser.addOption(delayOpt);
    parser.addOption(quitOpt);
    parser.addOption(sizeOpt);
    parser.process(app);

    // Validate every option before anything starts; bad values are errors,
    // never silently replaced by a default.
    MainWindowOptions options;
    if (parser.isSet(intervalOpt)) {
        const auto ms = cli::parseIntervalMs(parser.value(intervalOpt));
        if (!ms) {
            std::fprintf(stderr, "pulsescope: --interval must be one of 0.5, 1, 2 or 5 (got '%s')\n",
                         qPrintable(parser.value(intervalOpt)));
            return 2;
        }
        options.initialIntervalMs = *ms;
    }
    int page = -1;
    if (parser.isSet(pageOpt)) {
        const QStringList pages {QStringLiteral("cpu"),     QStringLiteral("memory"),       QStringLiteral("disks"),
                                 QStringLiteral("network"), QStringLiteral("temperatures"), QStringLiteral("processes")};
        page = static_cast<int>(pages.indexOf(parser.value(pageOpt).toLower()));
        if (page < 0) {
            std::fprintf(stderr, "pulsescope: unknown --page '%s'\n", qPrintable(parser.value(pageOpt)));
            return 2;
        }
    }
    std::optional<int> screenshotDelayMs;
    if (parser.isSet(screenshotOpt) || parser.isSet(delayOpt)) {
        screenshotDelayMs = cli::parseDelayMs(parser.value(delayOpt));
        if (!screenshotDelayMs) {
            std::fprintf(stderr, "pulsescope: --screenshot-delay must be a number of seconds from 0 to %.0f (got '%s')\n",
                         cli::kMaxDelaySeconds, qPrintable(parser.value(delayOpt)));
            return 2;
        }
    }
    std::optional<int> quitAfterMs;
    if (parser.isSet(quitOpt)) {
        quitAfterMs = cli::parseDelayMs(parser.value(quitOpt));
        if (!quitAfterMs) {
            std::fprintf(stderr, "pulsescope: --quit-after must be a number of seconds from 0 to %.0f (got '%s')\n",
                         cli::kMaxDelaySeconds, qPrintable(parser.value(quitOpt)));
            return 2;
        }
    }

    std::optional<QSize> size;
    if (parser.isSet(sizeOpt)) {
        size = cli::parseWindowSize(parser.value(sizeOpt));
        if (!size) {
            std::fprintf(stderr, "pulsescope: --size must be WIDTHxHEIGHT with each side from %d to %d (got '%s')\n",
                         cli::kMinWindowSide, cli::kMaxWindowSide, qPrintable(parser.value(sizeOpt)));
            return 2;
        }
    }

    if (::access("/proc/stat", R_OK) != 0) {
        QMessageBox::critical(nullptr, QStringLiteral("PulseScope"),
                              QStringLiteral("PulseScope needs the Linux /proc filesystem, but /proc/stat cannot be "
                                             "read. Is /proc mounted?"));
        return 1;
    }

    MainWindow window(std::move(options));
    if (page >= 0)
        window.showPage(static_cast<MainWindow::Page>(page));
    if (size)
        window.resize(*size);
    installSignalHandlers(&window);
    window.show();

    if (parser.isSet(screenshotOpt)) {
        const QString path = QFileInfo(parser.value(screenshotOpt)).absoluteFilePath();
        QTimer::singleShot(*screenshotDelayMs, &window, [&window, path] {
            QDir().mkpath(QFileInfo(path).absolutePath());
            if (!window.grab().save(path, "PNG"))
                std::fprintf(stderr, "pulsescope: could not save screenshot to %s\n", qPrintable(path));
            else
                std::fprintf(stderr, "pulsescope: screenshot saved to %s\n", qPrintable(path));
        });
    }
    if (quitAfterMs)
        QTimer::singleShot(*quitAfterMs, &window, &QWidget::close);
    return app.exec();
}
