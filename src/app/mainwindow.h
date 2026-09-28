// Main window: toolbar, metric navigation with live summaries, and one page
// per metric category. Owns the collector and the bounded history.
#pragma once

#include "app/collector.h"
#include "app/csvexporter.h"
#include "app/processmodel.h"
#include "app/windowowners.h"
#include "core/history.h"
#include "core/snapshot.h"

#include <QMainWindow>
#include <QPointer>
#include <QStringList>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class QAction;
class QActionGroup;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QSettings;
class QStackedWidget;
class QTimer;
class QTreeView;
class QSplitter;
class QTreeWidget;
class QWindow;
class CoreGrid;
class HeadRow;
class MetricButton;
class TimeChart;

struct MainWindowOptions {
    ps::SamplerOptions sampler;
    bool startCollector = true; // false: snapshots only arrive via applySnapshot()
    QString settingsFile;       // empty: the default per-user QSettings location
    int initialIntervalMs = 0;  // one of intervalChoicesMs(); 0: the saved setting
    CsvExporter::Writer csvWriter; // empty: atomic file save (tests may inject a writer)
    // Current time on the Snapshot::time clock (CLOCK_BOOTTIME seconds). A
    // snapshot is current only while its acquisition time is recent on this
    // clock, and charts end at it. Empty: ps::monotonicSeconds() when the
    // collector runs; with neither, injected snapshots count as current and
    // charts end at the newest sample (tests).
    std::function<double()> clock;
    // How the Qt platform theme was chosen at startup (shown in About).
    QString themeNote;
    // The Qt style chosen at startup, for About: a style sheet wraps it later.
    QString styleName;
    // Window-owner detection for the Apps group. The probe thread runs when
    // enabled and either the collector runs or a query is injected; without
    // it, results only arrive via applyWindowOwners(). Its clock defaults to
    // `clock`, which is then also called on the probe thread.
    windows::ProbeOptions windowProbe;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    enum Page { CpuPage, MemoryPage, DiskPage, NetworkPage, TemperaturePage, ProcessPage, PageCount };

    static const std::vector<int>& intervalChoicesMs(); // 500, 1000, 2000, 5000
    static const std::vector<int>& windowChoices();     // 60, 300, 900 seconds

    explicit MainWindow(MainWindowOptions options = {}, QWidget* parent = nullptr);
    ~MainWindow() override;

    void showPage(Page page);
    Page currentPage() const;
    void setPaused(bool paused);
    bool isPaused() const { return m_paused; }
    void setIntervalMs(int ms);
    int intervalMs() const { return m_intervalMs; }
    void setWindowSeconds(int seconds);
    int windowSeconds() const { return m_windowSeconds; }
    // Details off (the default) shows each page at a glance: the main value,
    // the main chart and the controls needed to choose what it shows. On, it
    // adds the full facts, secondary series and charts, tables and the
    // navigation's detail lines.
    // Only visibility and chart series change; nothing is collected
    // differently. Saved as the "details" setting.
    void setDetailsVisible(bool on);
    bool detailsVisible() const { return m_details; }
    // Smooth graphs (default on): between samples the visible charts scroll
    // with the clock at up to 60 frames per second; off, they move once per
    // sample. Saved as the "smoothGraphs" setting.
    void setSmoothGraphs(bool on);
    bool smoothGraphs() const { return m_smooth; }
    // Whether the frame clock is running right now (smooth graphs on, live,
    // and the window shown, exposed and not minimized).
    bool isAnimating() const;
    // Starts saving a copy of the retained history as CSV on the export
    // worker. Returns false (with a reason) if nothing can be exported yet
    // or an export is already running; csvExportFinished() reports the end.
    bool startCsvExport(const QString& path, QString* error = nullptr);
    bool isExporting() const { return m_exporter && m_exporter->isBusy(); }

    const ps::History& history() const { return m_history; }
    Collector* collector() const { return m_collector; }
    ps::SnapshotPtr latestSnapshot() const { return m_latest; }
    int snapshotsApplied() const { return m_applied; }
    bool isStale() const { return m_stale; }
    // The status bar says only what is wrong (empty, and the bar hidden, when
    // nothing is); the details are its tooltip and accessible description.
    QString statusText() const;
    QString statusDetail() const;

    // Feeds one snapshot as if the collector delivered it (tests, and the
    // collector connection itself). Ignored while paused.
    void applySnapshot(ps::SnapshotPtr snapshot);
    // Feeds one window-owner result as if the probe delivered it (tests, and
    // the probe connection itself). Ignored while paused or stopped, and if
    // resolved no later than the newest one applied or in the future.
    void applyWindowOwners(windows::ResultPtr result);
    // The Apps / Background split in effect (it follows the newest result
    // while that is valid; see README, "Processes").
    const ProcessGrouping& processGrouping() const { return m_grouping; }
    windows::WindowProbe* windowProbe() const { return m_probe; }
    // Stops the collector and window-probe threads and waits (within the
    // same budget, at most one second) for a running export; false if any
    // had to be abandoned.
    bool shutdown(int timeoutMs = 2000);

Q_SIGNALS:
    void snapshotApplied();
    void windowOwnersApplied();
    void csvExportFinished(bool ok, const QString& path, const QString& error);

protected:
    void closeEvent(QCloseEvent* event) override;
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void buildActions();
    void buildToolbar();
    void buildMenus();
    QWidget* buildNavigation();
    QWidget* buildCpuPage();
    QWidget* buildMemoryPage();
    QWidget* buildDiskPage();
    QWidget* buildNetworkPage();
    QWidget* buildTemperaturePage();
    QWidget* buildProcessPage();
    void loadSettings();
    void saveSettings();

    void updateSelectors(const ps::Snapshot& s);
    void updateDiskSelector(const ps::Snapshot& s);
    void updateNetSelector(const ps::Snapshot& s);
    void updateSensorList(const ps::Snapshot& s);
    void updateSummaries();
    void updateFacts();
    void updateFilesystems(const ps::Snapshot& s);
    void updateProcesses();
    // Recomputes m_grouping from the newest results and the clock; true if
    // it changed (the process model is regrouped when its page shows).
    bool updateGrouping();
    void updateGroupNote();
    void syncGroupExpansion();
    void onGroupExpansion(const QModelIndex& index, bool expanded);
    void saveGroupExpansion();
    // The process list is on screen, so window owners are asked for often.
    void fitProcessNameColumn();
    bool processListShown() const;
    void updateProbeVisibility();
    void updateStatus();
    void updateStatusBarVisibility();
    void updateChartNotices();
    void refreshCharts();
    void checkStale();
    void setStale(bool stale);
    double now() const;
    double staleLimitSeconds() const;
    bool isCurrent(const ps::Snapshot& s) const;
    bool isFromFuture(const ps::Snapshot& s) const;
    void applyDiskSelection();
    void applyNetSelection();
    void applySensorSelection();
    int sensorSlot(const QString& sensor);
    void exportCsvInteractive();
    void onCsvExportFinished(bool ok, const QString& path, const QString& error, qulonglong samples);
    void updateProcessCount();
    void showAbout();
    void updateHeadlines();
    void applyDetails();
    void applyCpuSeries();
    void applyMemorySeries();
    void applyScaledFonts();
    void updateFrameDriver();
    void advanceFrame();
    HeadRow* buildHeadline(Page page, const QString& prefix, QWidget* parent);
    QWidget* detailOnly(QWidget* widget);

    QString selectedDisk() const;
    QString selectedNet() const;
    std::vector<TimeChart*> allCharts() const;

    MainWindowOptions m_options;
    std::unique_ptr<QSettings> m_settings;
    Collector* m_collector = nullptr;
    windows::WindowProbe* m_probe = nullptr;
    CsvExporter* m_exporter = nullptr;
    ps::History m_history;
    ps::SnapshotPtr m_latest;
    int m_applied = 0;
    int m_intervalMs = 1000;
    int m_windowSeconds = 300;
    bool m_paused = false;
    bool m_stale = false;
    // Freshness, on the options.clock timeline: when the newest applied
    // sample of this run was acquired, and since when this run has waited
    // for its first one.
    double m_lastAcquired = ps::kNaN;
    double m_waitingSince = ps::kNaN;
    int m_graceIntervalMs = 1000; // longest interval in effect since the last sample
    QTimer* m_staleTimer = nullptr;

    // Selections requested by settings, applied once the device appears.
    QString m_wantedDisk;
    QString m_wantedNet;
    QStringList m_wantedSensors;
    bool m_sensorsChosen = false;
    // Colour and line style slot of each sensor that was plotted or shown in
    // the navigation, by sensor key, kept while the sensor is listed: a
    // sensor keeps its look when others are checked or unchecked.
    std::map<QString, int> m_sensorSlots;
    QString m_navSensor;

    QAction* m_pauseAction = nullptr;
    QAction* m_exportAction = nullptr;
    QAction* m_quitAction = nullptr;
    QAction* m_aboutAction = nullptr;
    QAction* m_detailsAction = nullptr;
    QAction* m_smoothAction = nullptr;
    std::vector<QAction*> m_pageActions;
    QActionGroup* m_intervalGroup = nullptr; // View > Update Interval
    QComboBox* m_rangeCombo = nullptr;

    std::vector<MetricButton*> m_nav;
    QWidget* m_navPanel = nullptr;
    QStackedWidget* m_stack = nullptr;
    QLabel* m_statusLabel = nullptr;

    TimeChart* m_cpuChart = nullptr;
    CoreGrid* m_coreGrid = nullptr;
    TimeChart* m_memChart = nullptr;
    TimeChart* m_swapChart = nullptr;
    TimeChart* m_diskChart = nullptr;
    QComboBox* m_diskCombo = nullptr;
    QTreeWidget* m_fsTable = nullptr;
    TimeChart* m_netChart = nullptr;
    QComboBox* m_netCombo = nullptr;
    TimeChart* m_tempChart = nullptr;
    QTreeWidget* m_sensorList = nullptr;
    QLabel* m_tempNote = nullptr;
    QLineEdit* m_procFilter = nullptr;
    QLabel* m_procCount = nullptr;
    QLabel* m_groupNote = nullptr;
    QTreeView* m_procTable = nullptr;
    int m_procNameWidth = 0;    // the Name column's width in Details
    int m_procNameMinWidth = 0; // Simple: narrower than this scrolls sideways
    bool m_procNameFitted = false; // Simple: the Name column fills the width
    ProcessModel* m_procModel = nullptr;
    ProcessFilterModel* m_procProxy = nullptr;
    // Window owners: the newest successful list and the newest failure
    // (each may be null), and the grouping derived from them.
    windows::ResultPtr m_ownersOk;
    windows::ResultPtr m_ownersFailure;
    double m_ownersNewest = ps::kNaN; // resolution time of the newest result applied
    // No result yet since the probe started or resumed (since when).
    bool m_ownersAwaiting = false;
    double m_ownersAwaitingSince = ps::kNaN;
    ProcessGrouping m_grouping;
    // Group expansion chosen by the user (saved), and while a filter is
    // typed: every group opens so matches show, unless collapsed during the
    // filter. Clearing the filter restores the saved state.
    std::array<bool, ProcessModel::GroupCount> m_groupExpanded {true, true, false};
    std::array<bool, ProcessModel::GroupCount> m_filterExpanded {true, true, true};
    bool m_syncingExpansion = false;

    // Fact labels by page, keyed by a stable id.
    std::vector<std::pair<std::string, QLabel*>> m_facts;
    QLabel* fact(const std::string& id) const;

    // Glance view and animation.
    bool m_details = false;
    int m_appliedDetails = -1; // state last applied: -1 none, 0 glance, 1 details
    bool m_smooth = true;
    QTimer* m_frameTimer = nullptr;
    QPointer<QWindow> m_watchedWindow;
    std::vector<QWidget*> m_detailWidgets; // shown only with Details
    std::array<HeadRow*, PageCount> m_head {};
    QSplitter* m_tempSplit = nullptr;
    QList<int> m_tempSplitSizes;
    bool m_shutdown = false;
};
