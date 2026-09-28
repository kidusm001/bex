// bex entry point.
#ifndef __linux__
#error "bex reads Linux /proc and /sys interfaces and only supports Linux."
#endif

#include "app/appicon.h"
#include "app/cli.h"
#include "app/mainwindow.h"
#include "app/metricbutton.h"
#include "app/settingsimport.h"
#include "app/timechart.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QMessageBox>
#include <QPaintEvent>
#include <QPixmap>
#include <QSocketNotifier>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#include <cerrno>
#include <optional>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

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

// Qt platform theme plugins mapped into this process, as evidence of which
// one Qt really loaded.
QStringList loadedThemePlugins()
{
    QStringList plugins;
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly | QIODevice::Text))
        return plugins;
    const QStringList lines = QString::fromLocal8Bit(maps.readAll()).split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const qsizetype slash = line.indexOf(QLatin1Char('/'));
        if (slash < 0)
            continue;
        const QString path = line.mid(slash);
        if ((path.contains(QStringLiteral("/platformthemes/")) || path.contains(QStringLiteral("/styles/")))
            && !plugins.contains(path))
            plugins << path;
    }
    return plugins;
}

void printThemeInfo(const QString& note, const QString& styleName)
{
    const QPalette pal = QApplication::palette();
    const QFont font = QApplication::font();
    std::printf("platform: %s\n", qPrintable(QGuiApplication::platformName()));
    std::printf("theme: %s\n", qPrintable(note));
    std::printf("style: %s\n", qPrintable(styleName));
    std::printf("font: %s\n", qPrintable(font.toString()));
    std::printf("icon theme: %s\n", qPrintable(QIcon::themeName()));
    std::printf("style sheet: %lld characters\n", static_cast<long long>(qApp->styleSheet().size()));
    std::printf("palette: window %s, window text %s, base %s, text %s, button %s, highlight %s\n",
                qPrintable(pal.color(QPalette::Window).name()), qPrintable(pal.color(QPalette::WindowText).name()),
                qPrintable(pal.color(QPalette::Base).name()), qPrintable(pal.color(QPalette::Text).name()),
                qPrintable(pal.color(QPalette::Button).name()), qPrintable(pal.color(QPalette::Highlight).name()));
    for (const QString& plugin : loadedThemePlugins())
        std::printf("loaded plugin: %s\n", qPrintable(plugin));
}

// --frame-stats: once a second, print how many frames the window really
// painted (update requests handled), what each cost on the GUI thread, and
// how often the charts and navigation entries painted and at what cost.
// Frames are counted, never inferred from the animation timer's interval.
class FrameStats : public QObject {
public:
    explicit FrameStats(QWidget* window)
        : QObject(window)
        , m_window(window)
    {
        m_window->installEventFilter(this);
        if (auto* driver = m_window->findChild<QTimer*>(QStringLiteral("frameTimer")))
            connect(driver, &QTimer::timeout, this, [this] {
                ++m_ticks;
                if (m_tick.isValid())
                    m_maxTickGap = std::max(m_maxTickGap, m_tick.nsecsElapsed() / 1e6);
                m_tick.start();
            });
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &FrameStats::report);
        timer->start(1000);
        m_elapsed.start();
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (event->type() == QEvent::UpdateRequest && !m_inFrame) {
            // Deliver it here to time the whole paint and flush.
            m_inFrame = true;
            QElapsedTimer t;
            t.start();
            QCoreApplication::sendEvent(object, event);
            m_costs.push_back(t.nsecsElapsed() / 1e6);
            m_inFrame = false;
            return true;
        }
        if (event->type() == QEvent::Paint && !m_inPaint) {
            // Chart and navigation entry paints are timed the same way.
            const bool chart = qobject_cast<TimeChart*>(object) != nullptr;
            if (!chart && !qobject_cast<MetricButton*>(object))
                return false;
            m_inPaint = true;
            QElapsedTimer t;
            t.start();
            QCoreApplication::sendEvent(object, event);
            const double ms = t.nsecsElapsed() / 1e6;
            m_inPaint = false;
            ++(chart ? m_chartPaints : m_navPaints);
            if (chart)
                for (const QRect& r : static_cast<QPaintEvent*>(event)->region())
                    m_chartArea += double(r.width()) * r.height();
            (chart ? m_chartMs : m_navMs) += ms;
            return true;
        }
        return false;
    }

private:
    void report()
    {
        // Charts (per-core ones come and go) and the native window appear
        // after start-up; installing a filter twice keeps one copy.
        for (auto* chart : m_window->findChildren<TimeChart*>())
            chart->installEventFilter(this);
        for (auto* button : m_window->findChildren<MetricButton*>())
            button->installEventFilter(this);
        if (QWindow* handle = m_window->windowHandle())
            handle->installEventFilter(this);
        const double secs = m_elapsed.nsecsElapsed() / 1e9;
        m_elapsed.restart();
        std::sort(m_costs.begin(), m_costs.end());
        auto at = [&](double q) { return m_costs.empty() ? 0.0 : m_costs[std::size_t(q * (m_costs.size() - 1))]; };
        std::fprintf(stderr,
                     "bex: frames %.1f/s, frame cost median %.2f ms, p95 %.2f ms, max %.2f ms; chart paints "
                     "%.1f/s (%.1f ms/s, %.0f kpx each); navigation paints %.1f/s (%.1f ms/s); animation ticks %.1f/s, longest gap "
                     "%.1f ms\n",
                     m_costs.size() / secs, at(0.5), at(0.95), m_costs.empty() ? 0.0 : m_costs.back(),
                     m_chartPaints / secs, m_chartMs / secs,
                     m_chartPaints ? m_chartArea / m_chartPaints / 1000 : 0.0, m_navPaints / secs, m_navMs / secs, m_ticks / secs,
                     m_maxTickGap);
        m_maxTickGap = 0;
        m_costs.clear();
        m_chartPaints = 0;
        m_navPaints = 0;
        m_chartMs = 0;
        m_chartArea = 0;
        m_navMs = 0;
        m_ticks = 0;
    }

    QWidget* m_window;
    QElapsedTimer m_elapsed;
    std::vector<double> m_costs;
    int m_chartPaints = 0;
    int m_navPaints = 0;
    double m_chartMs = 0;
    double m_chartArea = 0;
    double m_navMs = 0;
    bool m_inPaint = false;
    int m_ticks = 0;
    QElapsedTimer m_tick;
    double m_maxTickGap = 0;
    bool m_inFrame = false;
};

} // namespace

int main(int argc, char* argv[])
{
    // Decide the platform theme before QApplication reads the command line.
    // Requesting qt6ct affects this process only (see cli::chooseTheme).
    const cli::ThemeDecision theme = cli::chooseTheme(cli::probeThemeEnvironment(argc, argv));
    static char themeFlag[] = "-platformtheme";
    static char themeName[] = "qt6ct";
    std::vector<char*> args(argv, argv + argc);
    if (theme.requestQt6ct) {
        args.insert(args.begin() + 1, themeName);
        args.insert(args.begin() + 1, themeFlag);
    }
    int appArgc = static_cast<int>(args.size());
    args.push_back(nullptr);
    QApplication app(appArgc, args.data());
    if (theme.pluginMissing)
        std::fprintf(stderr, "bex: %s\n", qPrintable(theme.note));
    QApplication::setApplicationName(QStringLiteral("bex"));
    QApplication::setOrganizationName(QStringLiteral("bex"));
    QApplication::setApplicationVersion(QStringLiteral(BEX_VERSION));
    QApplication::setDesktopFileName(QStringLiteral(BEX_APP_ID));
    QApplication::setWindowIcon(bexIcon());
    // The style Qt chose; a style sheet applied later (qt6ct applies its own)
    // wraps it in an unnamed QStyleSheetStyle.
    const QStyle* startStyle = QApplication::style();
    const QString styleName =
        QStringLiteral("%1 (%2)").arg(startStyle->name(), QString::fromLatin1(startStyle->metaObject()->className()));

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
    const QCommandLineOption themeInfoOpt(QStringLiteral("theme-info"),
                                          QStringLiteral("Print the Qt platform theme, style, font and palette in use, "
                                                         "then exit."));
    const QCommandLineOption frameStatsOpt(QStringLiteral("frame-stats"),
                                           QStringLiteral("Print painted frames per second and their cost once a "
                                                          "second (for testing)."));
    parser.addOption(sizeOpt);
    parser.addOption(themeInfoOpt);
    parser.addOption(frameStatsOpt);
    parser.process(app);
    if (parser.isSet(themeInfoOpt)) {
        // Platform themes such as qt6ct apply fonts, palette and style sheets
        // from a queued call: report what the running app gets.
        QCoreApplication::processEvents();
        printThemeInfo(theme.note, styleName);
        return 0;
    }

    // Validate every option before anything starts; bad values are errors,
    // never silently replaced by a default.
    MainWindowOptions options;
    options.themeNote = theme.note;
    options.styleName = styleName;
    if (parser.isSet(intervalOpt)) {
        const auto ms = cli::parseIntervalMs(parser.value(intervalOpt));
        if (!ms) {
            std::fprintf(stderr, "bex: --interval must be one of 0.5, 1, 2 or 5 (got '%s')\n",
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
            std::fprintf(stderr, "bex: unknown --page '%s'\n", qPrintable(parser.value(pageOpt)));
            return 2;
        }
    }
    std::optional<int> screenshotDelayMs;
    if (parser.isSet(screenshotOpt) || parser.isSet(delayOpt)) {
        screenshotDelayMs = cli::parseDelayMs(parser.value(delayOpt));
        if (!screenshotDelayMs) {
            std::fprintf(stderr, "bex: --screenshot-delay must be a number of seconds from 0 to %.0f (got '%s')\n",
                         cli::kMaxDelaySeconds, qPrintable(parser.value(delayOpt)));
            return 2;
        }
    }
    std::optional<int> quitAfterMs;
    if (parser.isSet(quitOpt)) {
        quitAfterMs = cli::parseDelayMs(parser.value(quitOpt));
        if (!quitAfterMs) {
            std::fprintf(stderr, "bex: --quit-after must be a number of seconds from 0 to %.0f (got '%s')\n",
                         cli::kMaxDelaySeconds, qPrintable(parser.value(quitOpt)));
            return 2;
        }
    }

    std::optional<QSize> size;
    if (parser.isSet(sizeOpt)) {
        size = cli::parseWindowSize(parser.value(sizeOpt));
        if (!size) {
            std::fprintf(stderr, "bex: --size must be WIDTHxHEIGHT with each side from %d to %d (got '%s')\n",
                         cli::kMinWindowSide, cli::kMaxWindowSide, qPrintable(parser.value(sizeOpt)));
            return 2;
        }
    }

    if (::access("/proc/stat", R_OK) != 0) {
        QMessageBox::critical(nullptr, QStringLiteral("bex"),
                              QStringLiteral("bex needs the Linux /proc filesystem, but /proc/stat cannot be "
                                             "read. Is /proc mounted?"));
        return 1;
    }

    // Settings saved while the app was called PulseScope are copied once, as
    // the window is about to read its own: never over existing bex settings,
    // and never for the options that exit above.
    QString importFailure;
    if (options.settingsFile.isEmpty()) {
        const settingsimport::Result imported =
            settingsimport::importLegacy(settingsimport::legacyFile(), settingsimport::defaultFile());
        if (!imported.message.isEmpty())
            std::fprintf(stderr, "bex: %s\n", qPrintable(imported.message));
        if (imported.outcome == settingsimport::Outcome::Failed)
            importFailure = imported.message;
    }

    MainWindow window(std::move(options));
    if (page >= 0)
        window.showPage(static_cast<MainWindow::Page>(page));
    if (size)
        window.resize(*size);
    installSignalHandlers(&window);
    if (parser.isSet(frameStatsOpt))
        new FrameStats(&window);
    window.show();
    if (!importFailure.isEmpty())
        window.statusBar()->showMessage(importFailure, 15000);

    if (parser.isSet(screenshotOpt)) {
        const QString path = QFileInfo(parser.value(screenshotOpt)).absoluteFilePath();
        QTimer::singleShot(*screenshotDelayMs, &window, [&window, path] {
            QDir().mkpath(QFileInfo(path).absolutePath());
            if (!window.grab().save(path, "PNG"))
                std::fprintf(stderr, "bex: could not save screenshot to %s\n", qPrintable(path));
            else
                std::fprintf(stderr, "bex: screenshot saved to %s\n", qPrintable(path));
        });
    }
    if (quitAfterMs)
        QTimer::singleShot(*quitAfterMs, &window, &QWidget::close);
    return app.exec();
}
