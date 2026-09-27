# PulseScope

PulseScope is a native Linux performance monitor. It is a small Qt 6 Widgets
desktop app for keeping an eye on a machine while you work. It shows live
graphs and current values for:

- **CPU**: total and per-core utilization, split into user, system and I/O wait.
- **Memory**: RAM used, available and buff/cache, plus swap.
- **Disks**: read and write throughput per whole disk, and used/free capacity
  of mounted local filesystems.
- **Network**: receive and transmit throughput per interface.
- **Temperatures**: every hwmon and thermal-zone sensor the kernel exposes, in
  °C.
- **Processes**: a filterable, sortable, read-only table of PID, name, user,
  CPU, resident memory, threads, state, nice, disk I/O, age and command line.

Each page opens in a Simple view that reads at a glance: the main value, one
filled graph and a short summary. **Details** (Ctrl+D) adds every other
reading. Graphs scroll smoothly between samples.

It reads `/proc` and `/sys` directly. It needs no root, no daemon, no helper
processes, no network access and no dependencies beyond Qt.

## Requirements

- Linux. PulseScope reads Linux-specific kernel interfaces, and configuring
  the project on any other OS stops with an error.
- A C++20 compiler: GCC 11 or newer, or Clang 14 or newer.
- CMake 3.21 or newer. Ninja is optional but recommended.
- Qt 6.4 or newer: Core, Gui and Widgets, plus Test for the test suites.

### Arch Linux

```sh
sudo pacman -S --needed base-devel cmake ninja qt6-base
```

### Debian 12+ / Ubuntu 24.04+

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev
```

`qt6-base-dev` includes the Qt Test module. Ubuntu 22.04 ships Qt 6.2, which
is too old: use a newer release, or install Qt 6.4 or newer from another
source. The configure step checks the Qt version and stops if it is too old.

Running PulseScope needs only the Qt runtime libraries (`qt6-base` on Arch;
`libqt6widgets6` on Debian/Ubuntu) and a Qt platform plugin for your session.
Wayland and X11 both work. On Debian/Ubuntu Wayland sessions,
`qt6-wayland` gives native Wayland windows; without it, Qt falls back to
XWayland.

## Build, test and run

Release build (the version you would use day to day):

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULSESCOPE_BUILD_TESTS=OFF
cmake --build build/release
./build/release/pulsescope
```

Debug build with the test suites:

```sh
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPULSESCOPE_BUILD_TESTS=ON
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

The test suites:

| Test | What it covers |
| --- | --- |
| `tst_adversarial` | Parsers, delta math, history, the sampler over synthetic `/proc` and `/sys` trees, the collector thread, chart and widget rendering |
| `tst_core` | Identity, escaping and overflow guarantees |
| `tst_live` | Read-only invariants against this host's real `/proc` and `/sys`. It never assumes the host has any particular sensor, disk or interface. |
| `tst_ui` | The main window, Simple and Details views, process model, CSV export, frame driver, cached graph drawing and theme selection, run offscreen with isolated settings |
| `tst_graph_upgrade` | Smooth scrolling between samples, area fills, gaps and signed temperatures in the drawn pixels, and that animation never changes measurements |
| `desktop_entry` | Escaping of the installed desktop entry's `Exec` line |

- CTest runs the Qt tests with `QT_QPA_PLATFORM=offscreen`, so they need no
  display and never open windows on your desktop.
- Screenshots and other test output go to `build/<dir>/artifacts/`.
- `-L live` or `-L ui` selects one group, for example
  `ctest --test-dir build/debug -L ui`.

Useful CMake options:

- `-DPULSESCOPE_WERROR=ON` treats warnings as errors.
- `-DPULSESCOPE_SANITIZE="address;undefined"` builds with sanitizers. Use a
  separate build directory for this.

## Install (user-local)

```sh
cmake --install build/release --prefix ~/.local
```

This installs:

- `~/.local/bin/pulsescope`
- `~/.local/share/applications/io.github.kidusm001.PulseScope.desktop`
- `~/.local/share/icons/hicolor/scalable/apps/io.github.kidusm001.PulseScope.svg`
- `~/.local/share/metainfo/io.github.kidusm001.PulseScope.metainfo.xml`

The desktop entry's `Exec` line holds the absolute path of the installed
binary, so PulseScope appears in your application launcher even when
`~/.local/bin` is not on `PATH`. Some launchers only pick up new entries after
you log out and back in, or after running
`update-desktop-database ~/.local/share/applications`.

To uninstall, remove exactly the files recorded in the build tree's
`install_manifest.txt`:

```sh
cmake --build build/release --target uninstall
```

No step needs `sudo`. Installing to a system prefix such as `/usr/local` works
too, but is not required.

## Using PulseScope

Controls, left to right:

- **Navigation list**: pick a page. Each entry shows the current value and a
  small sparkline.
- **Toolbar**: pause/resume, the sampling interval (0.5, 1, 2 or 5 s), the
  graph window (1, 5 or 15 minutes), **Details** and CSV export.
- **View menu**: **Details** and **Smooth Graphs**.

The status bar shows the last sample time, the collector's own cost per
sample, and any problem reading a source.

Keyboard shortcuts:

| Key | Action |
| --- | --- |
| Ctrl+1 … Ctrl+6 | CPU, Memory, Disks, Network, Temperatures, Processes |
| P or Pause | Pause or resume |
| Ctrl+D | Show or hide details |
| Ctrl+E | Export history to CSV |
| Ctrl+F | Filter processes (switches to the Processes page) |
| Ctrl+Q | Quit |
| Left / Right, Home / End | Move the sample readout in a focused graph |
| Escape | Hide the graph readout |

Hovering over a graph shows the exact values at that moment. With a graph
focused, the arrow keys do the same.

Your choices are remembered in `~/.config/PulseScope/PulseScope.conf`: page,
interval, graph window, Details and Smooth Graphs, selected disk, interface
and sensors, process sort order, and window geometry.

### Simple view and Details

PulseScope starts in the Simple view: each page shows its main reading in
large type, one graph with a filled area under its line, and a short summary.
Turn on **Details** (toolbar, View menu or Ctrl+D) for everything else:

| Page | Simple | Details adds |
| --- | --- | --- |
| CPU | Total utilization | User, system and I/O wait lines, per-CPU graphs, load average and the other facts |
| Memory | Used of total, available, and the used area | Buff/cache, the swap graph and all memory facts |
| Disks | Read and write, with the disk picker | The local filesystem table, more I/O facts and notes |
| Network | Receive and transmit, with the interface picker | More interface facts and notes |
| Temperatures | The checked sensors, each named exactly in the legend and keeping its colour and line style (also in the navigation sparkline of the hottest sensor) | The sensor list for choosing what to plot, and notes |
| Processes | PID, name, CPU and memory (RSS), with filter and sort | User, threads, state, nice, disk I/O, age and command line |

Both views show the same live data. Switching never discards history,
selections or anything CSV export writes, and does not change how often
PulseScope samples. Warnings such as paused, stale, warming up or a missing
sensor are shown in both.

### Smooth graphs

Sampling and drawing are separate. The kernel is read once per sampling
interval; between samples the graphs scroll continuously with real time, at
up to 60 frames per second, so each point moves steadily to the left instead
of jumping once per sample.

- A new sample is drawn in along the straight line from the previous one,
  over at most 0.3 s (never more than half the interval). Nothing is drawn
  beyond the newest sample, lines are never smoothed, and peaks keep their
  height. A pause, suspend or outage is never bridged by a line or fill.
- Values, the hover readout and CSV export always use the measured
  timestamps and values. Animation only changes where the view ends.
- Animation runs only while the window can be seen and the view is live: it
  stops while the window is hidden, minimized or not exposed, and while
  paused. Frames copy cached images of the graphs and repaint only what
  moved; the graphs are redrawn from the data only when a sample arrives or
  the view changes.
- Turn off **View > Smooth Graphs** for less power use and less motion: the
  graphs then move once per sample. Smooth graphs on a visible window cost a
  few percent of one core; see [Design notes](#design-notes) for
  measurements. Qt 6 offers no system "reduce motion" preference for
  PulseScope to follow, so this switch is the way to choose.

### Look and theme

PulseScope uses your Qt style, palette, fonts and icons; graph colours are
chosen to suit the palette's lightness.

- An explicit choice always wins. `-platformtheme` or `QT_QPA_PLATFORMTHEME`
  selects the platform theme (the argument overrides the variable), and
  `-style` or `QT_STYLE_OVERRIDE` the style, exactly as for any Qt app. An
  empty `-platformtheme ''` also counts: Qt then ignores the variable and
  picks the theme itself. Arguments are read as Qt reads them, so on the xcb
  platform `-title`, `-geometry` and `-icon` take the next argument as their
  value. If that matters and the platform is not known before start-up (no
  `QT_QPA_PLATFORM`, `DISPLAY` or `WAYLAND_DISPLAY`), PulseScope leaves the
  theme to Qt and says the choice is not known. On desktops that integrate Qt
  themselves (KDE, GNOME, Xfce, LXQt and others) Qt's own choice is used.
- Some compositor sessions, such as Hyprland or Sway, leave
  `QT_QPA_PLATFORMTHEME` unset even when Qt apps are configured with qt6ct,
  so Qt apps started from a launcher get Qt's built-in look. When
  `~/.config/qt6ct/qt6ct.conf` exists and the qt6ct Qt 6 plugin is installed,
  PulseScope then asks for the qt6ct platform theme for itself only, by adding
  `-platformtheme qt6ct` to its own arguments. That brings in the style (for
  example Kvantum), palette, fonts, icon theme and style sheets configured in
  qt6ct. Nothing global changes: no environment of other programs and no
  configuration file is touched.
- If qt6ct is configured but its plugin is missing, PulseScope uses Qt's
  default look and says so on standard error, in **Help > About** and in
  `--theme-info`.
- `pulsescope --theme-info` prints the platform, how the theme was chosen,
  and the style, font, icon theme, style sheet size, main palette colours and
  platform theme plugin actually loaded.

### Choosing devices

- **Disks**: throughput is shown either for one whole disk or for "all
  physical disks". Partitions are never added to their parent disk, and
  device-mapper/virtual disks are never added to the physical ones, so no byte
  is counted twice. Loop and RAM disks are ignored.
- **Network**:
  - The default aggregate is "all physical interfaces". On hosts with no
    physical NIC (for example containers), it is "all non-loopback
    interfaces" instead.
  - Loopback and virtual interfaces (bridges, veth, tunnels) can still be
    selected individually. Each is labelled as loopback, virtual or physical.
- **Sensors**: check the sensors to plot. Your checks persist across restarts.
  A sensor that disappears, for example on hot-unplug, stays in the list
  marked as removed, checked or not, while the retained history still holds
  its readings. It drops out once its last reading ages out of the history.

Devices that appear or disappear while the app runs are picked up
automatically. Their graphs show gaps rather than zeros.

## What the numbers mean

- **Time axis**: graphs are plotted against real elapsed monotonic time
  (`CLOCK_BOOTTIME`), not a sample count.
  - A late sample stretches the line honestly.
  - A pause, suspend or collection outage leaves a visible gap instead of a
    line drawn through it.
- **Warm-up**:
  - CPU usage and all rates are differences between two readings, so the
    first sample, and the first sample after resume or a gap, shows "warming
    up" instead of a value.
  - Changing the interval does not restart this. The next rate covers the
    actual time between the two readings.
  - If the collector is delayed partway through a sample (for example when
    the system is starved), its counters are read at a different time than
    the sample's timestamp. That interval's disk, network and process rates
    are then left unavailable instead of being inflated. CPU usage is
    unaffected because it is a ratio of the kernel's own time counters.
  - A 32-bit counter that wraps is unwrapped. Any other counter that goes
    backwards (reset or device replacement) starts a new baseline instead of
    producing a spike.
- **CPU**:
  - Total and per-core utilization come from `/proc/stat`, as a percentage of
    the CPUs covered.
  - Guest time is counted once, inside user/nice.
  - "System" includes IRQ and softirq time.
  - Offline or hot-unplugged cores are shown as offline, not as 0%.
- **Memory**: the conventions match `free` from procps-ng 4:
  - *used* = MemTotal − MemAvailable.
  - *available* = the kernel's estimate of memory that can be given to new
    work without swapping (MemAvailable).
  - *buff/cache* = Buffers + Cached + SReclaimable. It overlaps with
    *available*, so used + buff/cache + available does not add up to the
    total.
  - Swap used = SwapTotal − SwapFree. "No swap" is shown when swap is
    disabled.
- **Rates**: bytes per second with IEC prefixes (KiB/s, MiB/s, GiB/s; 1 KiB =
  1024 B).
  - Disk throughput comes from `/proc/diskstats` sectors, which are always
    512 bytes in that file.
  - Network throughput comes from `/proc/net/dev`.
- **Filesystem capacity**:
  - Shown only for mounted local block filesystems such as ext4, btrfs, xfs,
    vfat and zfs. Several mount points of one filesystem (for example btrfs
    subvolumes) are shown as one row.
  - *Used* is the space in use. *Free* is the space available to
    unprivileged users. As with `df`, blocks reserved for root count as
    neither.
  - Known network, FUSE, autofs and pseudo filesystem types are skipped.
  - Capacity checks run on their own thread, isolated from collection and the
    UI. A check can still stall, for example when a mount changes while it
    runs, on a local filesystem backed by network storage, or on kernel I/O
    problems. Such a mount is reported as "not responding".
- **Temperatures**:
  - Celsius readings from `/sys/class/hwmon` (`temp*_input`) and
    `/sys/class/thermal`. Thermal zones that are also exposed through hwmon
    are listed once.
  - Sensors that report a fault or an implausible value are unavailable, not
    zero.
  - Having no sensors is normal in virtual machines and on some hardware. The
    Temperatures page then explains why.
- **Processes**: the table is read-only; PulseScope never signals or changes
  processes.
  - **CPU %** is per-process CPU time over the last interval, where **100% is
    one logical CPU fully busy**. A multi-threaded process can exceed 100%, up
    to 100% × the number of CPUs.
  - **Memory (RSS)** is resident set size. It includes shared pages, so the
    sum over processes can exceed used memory.
  - **Disk read/write** come from `/proc/PID/io`. The kernel only allows these
    for your own processes, so other users' processes show "—".
  - A reused PID is recognised as a new process via its start time and never
    inherits the old process's counters.
  - Kernel threads are hidden unless "Show kernel threads" is checked.
- **Unavailable data**:
  - A value that could not be read is shown as "—" or "n/a" and left as a gap
    in the graphs. It is never shown as 0.
  - Freshness is judged by when a sample was taken, not when it reached the
    window. If no recent sample exists, for example because a kernel read is
    blocked, the whole view is marked stale and the graphs keep moving so the
    outage shows as a growing gap. The view goes live again only when a
    recent sample arrives.

History keeps the last 15 minutes, at most 1800 samples per series, whatever
graph window you are viewing. Changing the graph window never discards data.
Changing the interval keeps existing history, and rates continue across the
change without a new warm-up.

### Pause

Pause stops sampling and freezes the view. You can still hover graphs, switch
pages, change the graph window and export. On resume, counter baselines are
reset, so the first interval afterwards is warm-up. The paused period stays as
a gap in the graphs.

### CSV export

**Export history** (Ctrl+E) writes the whole retained history of every
recorded series to a CSV file.

- **Columns**: `time_since_boot_s`, `unix_time_s`, `continuous`, then one
  column per series. Each header carries its unit, for example `CPU total (%)`,
  `Memory used (bytes)`, `Disk nvme0n1 read (B/s)` or `… (°C)`.
- **Values**: a cell is empty when the value was unavailable. `continuous` is 0
  for the first sample after a gap.
- **Writing**: the file is written in the background and replaced atomically,
  so a failed or interrupted save never leaves a partial file at the chosen
  name. Errors are reported in the window.

### History limits

On machines with very many devices, per-device history is capped:

| Series | Limit |
| --- | --- |
| Per-core CPU series | 256 |
| Disk, interface or sensor series, each | 80 |

Live values are still shown for devices over the limit. Their graphs say that
they are not recorded.

### Command-line options

```text
pulsescope [--interval 0.5|1|2|5]
           [--page cpu|memory|disks|network|temperatures|processes]
           [--size WIDTHxHEIGHT]
           [--screenshot FILE.png [--screenshot-delay SECONDS]]
           [--quit-after SECONDS]
           [--theme-info]
           [--frame-stats]
```

- `--interval` replaces the saved interval at startup. Like a change made in
  the toolbar, it is saved and used on the next start.
- `--page`, `--size`, `--screenshot` and `--quit-after` mainly exist for
  testing and documentation screenshots.
- `--theme-info` prints the Qt theme, style, font, icon theme and palette in
  use, then exits (see [Look and theme](#look-and-theme)).
- `--frame-stats` prints, once a second, the frames the window actually
  painted, what they cost on the UI thread, and how often the graphs and
  navigation entries repainted. It counts real paints; it does not infer a
  frame rate from the animation timer.
- Invalid values are rejected with exit status 2 instead of being silently
  replaced.

## Privacy and permissions

- PulseScope runs as your normal user and never asks for or uses elevated
  privileges.
- It makes no network connections, has no telemetry and writes nothing except
  its settings file and the CSV files you explicitly export.
- It only reads files under `/proc` and `/sys`, and asks the kernel for
  filesystem capacity (`statvfs`) of local mounts.
- It does not run subprocesses, tune the system or generate load.
- Everything it shows is what any user on the machine can already read.
  - Some per-process fields, such as disk I/O counters, are restricted by the
    kernel for other users' processes. Those show as unavailable.
  - Hardened kernels (for example with `hidepid`) show fewer processes.

## Limitations

- Linux only, by design.
- Per-process I/O counters are unavailable for other users' processes without
  privileges. PulseScope does not try to work around this.
- **Disk throughput**:
  - Throughput is per whole block device. Per-partition and per-filesystem
    I/O rates are not shown.
  - Stacked devices (LVM, dm-crypt, md RAID) can be selected individually but
    are excluded from the "all physical disks" total.
- Filesystem capacity is not shown for network, FUSE or other remote
  filesystems.
- Sensor names come from the kernel driver. Some chips report only generic
  labels such as `temp1`.
- History is kept in memory only and is lost when the app quits. Use CSV
  export to keep it.
- CSV `unix_time_s` is computed at export time from the current difference
  between the wall clock and the boot-time clock. If the wall clock was
  stepped during the retained history (NTP or a manual change), earlier
  timestamps shift by that step. `time_since_boot_s` is always exact.

## Design notes

- **Structure**:
  - A Qt-free core (`src/core`) parses kernel files, does delta and rate math
    and keeps bounded histories. It is tested with synthetic `/proc` and
    `/sys` trees.
  - The UI (`src/app`) receives immutable snapshots from a background
    collector thread over queued signals.
  - The UI thread never reads `/proc` or `/sys` and never checks filesystem
    capacity. Its only file access is the settings file, written
    synchronously when a setting changes and at exit, and the CSV file
    chooser, which is synchronous and can be slow on an unresponsive mount.
    The CSV file itself is written on a worker thread.
  - When the window closes, PulseScope waits up to two seconds in total for
    its worker threads: first the collector, which gives its capacity-check
    thread 200 ms, then any running export, for at most one second of what
    remains. A worker still stuck in a blocked kernel call after that is
    abandoned instead of waited for.
  - This limits PulseScope's own waiting, not the process exit. Saving
    settings is synchronous, and the kernel may delay the exit of a process
    whose thread is stuck in uninterruptible I/O.
- **Look**: the interface follows the system palette, fonts and light/dark
  preference. It uses plain native controls and custom `QPainter` graphs, with
  no web view, QML or chart library.
- **Graph drawing**:
  - One shared timer drives animation for the visible page and the navigation
    list, at about 60 Hz (15 Hz on the Processes page, whose only moving parts
    are the sparklines). It never samples, sorts, rebuilds widgets or updates
    screen-reader text; those happen once per sample.
  - Each graph keeps its plot, grid and data rasterized in a few opaque
    images, one per sub-pixel phase (quarter or eighth pixel). A frame copies
    the right image at a whole-pixel offset and draws the lines that stay
    fixed on screen (time grid, frame) over it. The images are redrawn only
    when a sample arrives, the value range or size changes, or the palette,
    font or style changes.
  - A frame repaints only what scrolling can change. Above the data nothing
    does. Below lines that stay in a narrow band, only the columns where a
    filled area, outage shading or a sample being drawn in has a vertical
    edge change, so a frame repaints that band and those columns; otherwise
    it repaints the rows from the highest drawn point down. A hover or
    keyboard readout adds its line and box, at their old and new places.
    Each repainted rectangle is copied on its own: copying through a clip of
    several rectangles makes Qt's raster engine blend row by row, spread
    over its thread pool, which costs more than it saves.
  - A frame is painted only when a graph moved by at least a sub-pixel
    step, so a 5-minute window at the default size paints about 35 to 40
    frames per second and a 1-minute window about 60.
  - Measured on this project's 8-core development machine, Release build,
    1 s sampling, all threads, one core = 100%, while the desktop was in use
    (load average 1.2 to 2.4):
    - Offscreen platform, 1180×780 window: Simple CPU page 3.5% with the
      default 5-minute window, 3.7% with a 1-minute window; Simple Memory
      page 4.0% (1 minute); Details CPU page with eight per-core graphs 4.6%
      (1 minute); smooth graphs off 1.3%. 56 to 68 MiB resident.
    - A visible native Wayland window (Hyprland, 1.2 scale factor, tiled at
      788×858): Simple CPU page about 5.5% with the 5-minute window (about 35
      painted frames per second) and 6 to 8% with a 1-minute window (about
      64). The extra over offscreen is spent handing frames to the
      compositor, on the UI thread and Qt's Wayland event threads. With the
      window on a hidden workspace, painting stops and the app uses about
      1.1%, mostly the collector thread's sampling and process scan.
    - The cost grows with how much of the plot the data fills.
- **Accessibility**: data series are told apart by legend, line style and
  colour, never colour alone. Every control has a tooltip and an accessible
  name, and graphs expose their current values to screen readers.
