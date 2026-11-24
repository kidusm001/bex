# bex

bex is a native Linux performance monitor. It is a small Qt 6 Widgets
desktop app for keeping an eye on a machine while you work. It shows live
graphs and current values for:

- **CPU**: total and per-core utilization, split into user, system and I/O wait.
- **Memory**: RAM used, available and buff/cache, plus swap.
- **Disks**: read and write throughput per whole disk, and used/free capacity
  of mounted local filesystems.
- **Network**: receive and transmit throughput per interface.
- **Temperatures**: every hwmon and thermal-zone sensor the kernel exposes, in
  °C.
- **Processes**: a filterable, sortable list of PID, name, user, CPU,
  resident memory, threads, state, nice, disk I/O, age and command line,
  grouped like a task manager into apps, background processes and kernel
  processes. **End task** asks one of your own processes to quit.

Each page opens in a Simple view that reads at a glance: the main value, one
filled graph and a short summary. **Details** (Ctrl+D) adds every other
reading. Graphs scroll smoothly between samples.

It reads `/proc` and `/sys` directly. It needs no root, no daemon, no helper
processes, no network access and no dependencies beyond Qt (plus, optionally,
libxcb for telling apps apart on X11).

## Install

Pre-built packages and checksums (`SHA256SUMS.txt`) are published on the
[GitHub Releases](https://github.com/kidusm001/bex/releases) page, and native
`pacman`, `dnf`, and `apt` repositories are hosted at
<https://kidusm001.github.io/bex/>:

- **Any Linux distro (AppImage)**: download `bex-<version>-x86_64.AppImage`,
  make it executable (`chmod +x`), and run it.
- **Arch Linux**: add the hosted `pacman` repo from
  <https://kidusm001.github.io/bex/> (or install the `.pkg.tar.zst` asset via
  `sudo pacman -U ./bex-*.pkg.tar.zst`).
- **Fedora**:
  ```sh
  sudo dnf config-manager addrepo --from-repofile=https://kidusm001.github.io/bex/rpm/bex.repo
  sudo dnf install bex
  ```
  (or download the `.rpm` asset and run `sudo dnf install ./bex-*.x86_64.rpm`).
- **Ubuntu 24.04+ / Debian 12**: download the matching `.deb` asset from GitHub
  Releases (`noble` for Ubuntu 24.04+/Mint 22+, `bookworm` for Debian 12) and
  install it with `sudo apt install ./bex_*.deb`, or add the APT repository from
  <https://kidusm001.github.io/bex/>.
- **NixOS / Nix**:
  ```sh
  nix run github:kidusm001/bex
  ```

See [docs/RELEASING.md](docs/RELEASING.md) for the CI/CD pipeline and release
publishing instructions. Licensed under the [MIT License](LICENSE).

## Requirements

- Linux. bex reads Linux-specific kernel interfaces, and configuring
  the project on any other OS stops with an error.
- A C++20 compiler: GCC 11 or newer, or Clang 14 or newer.
- CMake 3.21 or newer. Ninja is optional but recommended.
- Qt 6.4 or newer: Core, Gui and Widgets, plus Test for the test suites.
- Optional: `xcb` and `xcb-res` (libxcb 1.14 or newer, found with
  `pkg-config`) to find app windows on X11. Without them bex builds
  and runs the same, and on X11 the Processes page says app detection is
  unavailable. Hyprland needs nothing extra.

### Arch Linux

```sh
sudo pacman -S --needed base-devel cmake ninja qt6-base libxcb
```

### Debian 12+ / Ubuntu 24.04+

```sh
sudo apt install build-essential cmake ninja-build qt6-base-dev libxcb1-dev libxcb-res0-dev
```

`qt6-base-dev` includes the Qt Test module. Ubuntu 22.04 ships Qt 6.2, which
is too old: use a newer release, or install Qt 6.4 or newer from another
source. The configure step checks the Qt version and stops if it is too old.

Running bex needs only the Qt runtime libraries (`qt6-base` on Arch;
`libqt6widgets6` on Debian/Ubuntu), `libxcb` and `libxcb-res` if it was built
with them, and a Qt platform plugin for your session.
Wayland and X11 both work. On Debian/Ubuntu Wayland sessions,
`qt6-wayland` gives native Wayland windows; without it, Qt falls back to
XWayland.

## Build, test and run

Get the source:

```sh
git clone https://github.com/kidusm001/bex.git
cd bex
```

Release build (the version you would use day to day):

```sh
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBEX_BUILD_TESTS=OFF
cmake --build build/release
./build/release/bex
```

Debug build with the test suites:

```sh
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBEX_BUILD_TESTS=ON
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

The test suites:

| Test | What it covers |
| --- | --- |
| `tst_adversarial` | Parsers, delta math, history, the sampler over synthetic `/proc` and `/sys` trees, the collector thread, chart and widget rendering |
| `tst_core` | Identity, escaping and overflow guarantees |
| `tst_live` | Read-only invariants against this host's real `/proc` and `/sys`. It never assumes the host has any particular sensor, disk or interface. |
| `tst_ui` | The main window, Simple and Details views, grouped process list, CSV export, frame driver, cached graph drawing and theme selection, run offscreen with isolated settings |
| `tst_process_groups` | Grouping processes into apps, background and kernel: the compositor reply parser, identity checks against synthetic `/proc` trees, the window probe thread and the grouped model |
| `tst_council` | Review regressions: a capped read is not a complete table, per-process read timing, refreshing process metadata, worker threads that cannot start, and per-core graph accessibility |
| `tst_graph_upgrade` | Smooth scrolling between samples, area fills, gaps and signed temperatures in the drawn pixels, and that animation never changes measurements |
| `desktop_entry` | Escaping of the installed desktop entry's `Exec` line |
| `tst_settings_import`, `tst_rename_adversarial` | The one-time import of settings saved under the former name PulseScope, and the app's name in its command line, messages and window, each with a private settings folder |

- CTest runs the Qt tests with `QT_QPA_PLATFORM=offscreen`, so they need no
  display and never open windows on your desktop.
- Screenshots and other test output go to `build/<dir>/artifacts/`.
- Each test carries labels, and `-L` or `-LE` selects by them:

  | Label | Meaning | Tests |
  | --- | --- | --- |
  | `unit` | Synthetic `/proc` and `/sys` trees, injected clocks, local test sockets, at most the test's own `/proc` entries | `tst_adversarial`, `tst_core`, `tst_council`, `tst_process_groups`, `desktop_entry`, `tst_settings_import`, `tst_rename_adversarial` |
  | `ui` | Widgets, offscreen | `tst_ui`, `tst_process_groups`, `tst_graph_upgrade`, `tst_settings_import` |
  | `host` | Also depends on this machine: its mount table, real `/proc` and `/sys`, process limits and privileges, or your Qt theme files | `tst_adversarial`, `tst_core`, `tst_council`, `tst_live`, `tst_ui`, `tst_graph_upgrade`, `tst_settings_import`, `tst_rename_adversarial` |
  | `live` | Only this host's real `/proc` and `/sys` | `tst_live` |
  | `slow` | Several minutes | `tst_graph_upgrade` |
  | `install` | Installed files | `desktop_entry` |

  `ctest --test-dir build/debug -L unit -LE host` runs the subset that does
  not depend on the machine, and `-LE slow` skips the long graph suite. A
  label marks a whole binary, so most `host` suites are largely synthetic
  with one or a few host cases, and their skips (for example no qt6ct
  configuration, or running as root) depend on the machine.

Useful CMake options:

- `-DBEX_WERROR=ON` treats warnings as errors.
- `-DBEX_SANITIZE="address;undefined"` builds with sanitizers. Use a
  separate build directory for this. Sanitizer reports are not recoverable,
  so any report, including undefined behaviour, fails the test that hit it.
  For the slower code, the CTest time limit for `tst_graph_upgrade` grows
  from 1800 s to 3600 s and its Qt Test limit per test function from 300 s
  to 1800 s; the other suites keep 120 s.
- `-DBEX_X11=AUTO|ON|OFF` controls X11 app detection. `AUTO` (the
  default) uses `xcb` and `xcb-res` when `pkg-config` finds them, `ON` stops
  with an error when they are missing, `OFF` leaves them out. The value is
  case-insensitive; anything else stops with an error.

## Install (user-local)

```sh
cmake --install build/release --prefix ~/.local
```

This installs:

- `~/.local/bin/bex`
- `~/.local/share/applications/io.github.kidusm001.bex.desktop`
- `~/.local/share/icons/hicolor/scalable/apps/io.github.kidusm001.bex.svg`
- `~/.local/share/metainfo/io.github.kidusm001.bex.metainfo.xml`

The desktop entry's `Exec` line holds the absolute path of the installed
binary, so bex appears in your application launcher even when
`~/.local/bin` is not on `PATH`. Some launchers only pick up new entries after
you log out and back in, or after running
`update-desktop-database ~/.local/share/applications`.

To uninstall, remove exactly the files recorded in the build tree's
`install_manifest.txt`:

```sh
cmake --build build/release --target uninstall
```

bex used to be called PulseScope. Installing bex does not remove an earlier
PulseScope install, which would stay in the launcher as a second entry. Run
the `uninstall` target in the build tree that installed PulseScope, or remove
its four files:

- `~/.local/bin/pulsescope`
- `~/.local/share/applications/io.github.kidusm001.PulseScope.desktop`
- `~/.local/share/icons/hicolor/scalable/apps/io.github.kidusm001.PulseScope.svg`
- `~/.local/share/metainfo/io.github.kidusm001.PulseScope.metainfo.xml`

Its settings are kept and imported once; see
[Settings from PulseScope](#settings-from-pulsescope).

No step needs `sudo`. Installing to a system prefix such as `/usr/local` works
too, but is not required.

## Using bex

Controls, left to right:

- **Navigation list**: pick a page. Each entry shows the current value and a
  small sparkline; with **Details** on, a third line adds a secondary value.
- **Toolbar**: **History** (the graph window: 1, 5 or 15 minutes) and
  **Details**.
- **File menu**: **Export History as CSV** and **Quit**.
- **View menu**: **Pause**, **Update Interval** (0.5, 1, 2 or 5 seconds),
  **Details**, **Smooth Graphs** (Ctrl+Shift+M) and the pages.
- **Processes page**: the filter, **End task…** for the selected process, and
  the process count. Right-clicking a process (or the Menu key or Shift+F10)
  offers **End task…** and **Force end task…**; see
  [Ending a process](#ending-a-process).

The status bar appears only when something needs attention: paused, readings
unavailable or out of date, a collector that cannot start, or a history limit
reached. It shows a few words; its tooltip (and accessible description) gives
the full reason. When everything is healthy it is hidden.

Units and conventions (what "used" memory means, CPU per core, which devices
are counted) are in the tooltips of the headings, pickers and table columns
rather than in notes on the pages.

Keyboard shortcuts:

| Key | Action |
| --- | --- |
| Ctrl+1 … Ctrl+6 | CPU, Memory, Disks, Network, Temperatures, Processes |
| P or Pause | Pause or resume |
| Ctrl+D | Show or hide details |
| Ctrl+Shift+M | Turn smooth graphs on or off |
| Ctrl+E | Export history to CSV |
| Ctrl+F | Filter processes (switches to the Processes page) |
| Delete in the process list | End the selected process, after confirmation |
| Ctrl+Q | Quit |
| Left / Right, Home / End | Move the sample readout in a focused graph |
| Escape | Hide the graph readout |
| Left / Right on a process group | Collapse or expand the group |
| Typing a name in the process list | Jump to the next process whose name starts with it (open groups only) |

Hovering over a graph shows the exact values at that moment. With a graph
focused, the arrow keys do the same.

Your choices are remembered in `~/.config/bex/bex.conf` (under
`$XDG_CONFIG_HOME` when that is set): page, interval, graph window, Details
and Smooth Graphs, selected disk, interface and sensors, process sort order,
which process groups are open, and window geometry.

### Settings from PulseScope

bex used to be called PulseScope, which kept its settings in
`~/.config/PulseScope/PulseScope.conf`. The first time bex opens its window
without a `bex.conf`, it copies every setting from that file into a new
`bex.conf` and prints one line saying so. After that it never reads the old
file again:

- An existing `bex.conf` always wins. It is never overwritten or merged,
  even when it is empty.
- The old file is only read. It is never changed, moved or deleted; remove
  it yourself once you no longer need it.
- `--help`, `--version`, `--theme-info` and rejected options exit before any
  settings are read, so they neither import nor create anything.
- If the old file cannot be found for certain (a folder on its path cannot
  be searched), cannot be read, is damaged or is larger than 8 MiB, or
  `bex.conf` cannot be written, nothing is imported. bex prints why to its
  standard error, shows it in the status bar for 15 seconds, and starts with
  its default settings, which it then saves as its own: fix the problem and
  delete `bex.conf` to import again.
- Deleting `bex.conf` while the old file still exists makes the next start
  import it again.

### Simple view and Details

bex starts in the Simple view: each page shows its main reading in
large type and one graph with a filled area under its line. A line of text
under the reading appears only where it adds something (memory of total and
swap, root filesystem free space, the hottest sensor) or when a reading is
paused or unavailable.
Turn on **Details** (toolbar, View menu or Ctrl+D) for everything else:

| Page | Simple | Details adds |
| --- | --- | --- |
| CPU | Total utilization | User, system and I/O wait lines, per-CPU graphs, load average and the other facts |
| Memory | Used of total, available, and the used area | Buff/cache, the swap graph and all memory facts |
| Disks | Read and write, with the disk picker | The local filesystem table and more I/O facts |
| Network | Receive and transmit, with the interface picker | More interface facts |
| Temperatures | The checked sensors, each named exactly in the legend and keeping its colour and line style (also in the navigation sparkline of the hottest sensor) | The sensor list for choosing what to plot |
| Processes | Name, PID, CPU and memory (RSS) in the three groups, with filter and sort | User, threads, state, nice, disk I/O, age and command line |

Both views show the same live data. Switching never discards history,
selections or anything CSV export writes, and does not change how often
bex samples. Warnings such as paused, stale, warming up or a missing
sensor are shown in both.

### Ending a process

Like a task manager's End task, bex can end one process: the one you
select, never its children, its group or other processes with the same name.

- **End task…** (the button, Delete in the list, or the row's context menu)
  sends `SIGTERM`: the process is asked to quit and may save or clean up
  first.
- **Force end task…** (context menu only) sends `SIGKILL`, which cannot be
  caught or ignored: the process cannot save or clean up. bex never
  escalates from one to the other by itself.
- Both ask first, naming the process and its PID. Cancel is the default, and
  Escape cancels. Unsaved work in the process may be lost.
- A short status message says the signal was *sent*; it does not mean the
  process has ended yet. The row goes away when a later sample no longer
  lists it (after resuming, if the list is paused). If nothing could be sent,
  a message says why.
- Only your own processes can be ended: the controls are disabled for other
  users' processes, processes whose owner is unknown, kernel threads, PID 1
  and bex itself. bex never asks for more privileges.
- Just before sending, bex checks again that the PID still names the
  process you chose: same start time, name and owner, still running, and not
  a kernel thread. The signal goes through a process file descriptor
  (`pidfd`) opened before that check, so if the process has exited and its
  PID been reused, the newcomer cannot receive it. Without pidfd support
  (Linux 5.3 or later) nothing is sent; there is no fallback to a signal by
  PID number.

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
- Turn off **View > Smooth Graphs** (Ctrl+Shift+M) for less power use and
  less motion: the graphs then move once per sample. Smooth graphs on a
  visible window cost a few percent of one core; see
  [Design notes](#design-notes) for measurements. Qt 6 offers no system
  "reduce motion" preference for bex to follow, so this switch is the
  way to choose.

### Look and theme

bex uses your Qt style, palette, fonts and icons; graph colours are
chosen to suit the palette's lightness.

- An explicit choice always wins. `-platformtheme` or `QT_QPA_PLATFORMTHEME`
  selects the platform theme (the argument overrides the variable), and
  `-style` or `QT_STYLE_OVERRIDE` the style, exactly as for any Qt app. An
  empty `-platformtheme ''` also counts: Qt then ignores the variable and
  picks the theme itself. Arguments are read as Qt reads them, so on the xcb
  platform `-title`, `-geometry` and `-icon` take the next argument as their
  value. If that matters and the platform is not known before start-up (no
  `QT_QPA_PLATFORM`, `DISPLAY` or `WAYLAND_DISPLAY`), bex leaves the
  theme to Qt and says the choice is not known. On desktops that integrate Qt
  themselves (KDE, GNOME, Xfce, LXQt and others) Qt's own choice is used.
- Some compositor sessions, such as Hyprland or Sway, leave
  `QT_QPA_PLATFORMTHEME` unset even when Qt apps are configured with qt6ct,
  so Qt apps started from a launcher get Qt's built-in look. When
  `~/.config/qt6ct/qt6ct.conf` exists and the qt6ct Qt 6 plugin is installed,
  bex then asks for the qt6ct platform theme for itself only, by adding
  `-platformtheme qt6ct` to its own arguments. That brings in the style (for
  example Kvantum), palette, fonts, icon theme and style sheets configured in
  qt6ct. Nothing global changes: no environment of other programs and no
  configuration file is touched.
- If qt6ct is configured but its plugin is missing, bex uses Qt's
  default look and says so on standard error, in **Help > About** and in
  `--theme-info`.
- `bex --theme-info` prints the platform, how the theme was chosen,
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
  - The same applies to each process on its own. A process whose counters
    were read noticeably earlier or later within the sample than last time
    (for example after a slow read earlier in the process scan) shows "—" for
    its CPU and disk rates for that interval, while other processes and the
    system rates keep their values.
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
  - From 90% use (used / (used + free)) the usage bar turns red and the
    row's tooltip says "Nearly full".
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
- **Processes**: bex signals a process only when you confirm End task
  or Force end task for it (see [Ending a process](#ending-a-process)); it
  never changes processes otherwise.
  - **CPU %** is per-process CPU time over the last interval, where **100% is
    one logical CPU fully busy**. A multi-threaded process can exceed 100%, up
    to 100% × the number of CPUs.
  - **Memory (RSS)** is resident set size. It includes shared pages, so the
    sum over processes can exceed used memory.
  - **Disk read/write** come from `/proc/PID/io`. The kernel only allows these
    for your own processes, so other users' processes show "—".
  - A reused PID is recognised as a new process via its start time and never
    inherits the old process's counters.
  - **User** and **Command** are read when a process first appears or
    changes its name, and then re-read every 5 s, staggered by PID so the
    first refresh comes 5 to 10 s after a process appears. A running process
    that changes its real UID or rewrites its command line therefore shows the
    change within about 10 s. If one of these reads fails, the user is shown
    as "—" and the command as `[name]` rather than keeping the old value, and
    the read is tried again at the next sample. CPU, memory and disk figures
    are still read at every sample.
  - See [Process groups](#process-groups) for how processes are grouped.
- **Unavailable data**:
  - A value that could not be read is shown as "—" or "n/a" and left as a gap
    in the graphs. It is never shown as 0.
  - Kernel tables (`/proc/diskstats`, `/proc/net/dev`, mountinfo and the
    like) are read whole, up to 8 MiB. A larger one is reported as "larger
    than 8 MiB; not read" and its devices are unavailable, instead of a
    partial table passing for the whole system.
  - Freshness is judged by when a sample was taken, not when it reached the
    window. If no recent sample exists, for example because a kernel read is
    blocked, the whole view is marked stale and the graphs keep moving so the
    outage shows as a growing gap. The view goes live again only when a
    recent sample arrives.

History keeps the last 15 minutes, at most 1800 samples per series, whatever
graph window you are viewing. Changing the graph window never discards data.
Changing the interval keeps existing history, and rates continue across the
change without a new warm-up.

### Process groups

The Processes page lists every process once, in three groups that always
stay in this order, whichever column you sort by:

- **Apps**: processes that own a window on your desktop, including windows
  that are minimized, unfocused, or on a hidden or special workspace. A
  process with several windows is listed once. Only the process the
  compositor names as the window's owner counts. Helpers without windows of
  their own, such as a browser's content processes, stay in Background:
  bex does not guess family ties from parent PIDs or names.
- **Background processes**: every other user-space process.
- **Kernel processes**: kernel threads, which the kernel marks itself
  (`PF_KTHREAD` in `/proc/PID/stat`).

Group headers count processes. They do not add up CPU or memory, which would
double-count shared memory and mislead. Within each group, rows sort by the
chosen column; equal values fall back to PID. The filter searches PID, name,
user and command line in every group. While it is active, headers show
"matching of total", every group opens so matches are never hidden in a
closed group, and clearing the filter restores the groups you had opened or
closed. Apps and Background start open and Kernel starts closed; your choice
is remembered. If you had "Show kernel threads" checked in an earlier
version, Kernel starts open instead. Selection, scroll position and sort
order survive every refresh and a process moving between groups. A reused
PID is a new process: it inherits neither the old one's selection nor its
window.

Finding app windows depends on the desktop:

| Desktop | How |
| --- | --- |
| Hyprland | Asks Hyprland for its window list over its own IPC socket (`$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock`, request `j/clients`). This is read-only: bex never focuses, moves or changes windows. |
| X11 | Reads the window manager's `_NET_CLIENT_LIST` and asks the X server which process owns each window (the X-Resource extension), when built with `xcb` and `xcb-res` and connected to a local X server. |
| Other Wayland desktops | Not supported yet. |

When the window list cannot be read, the Apps header reads **Apps
(unavailable)**, and a note above the list and the header's tooltip say why.
User processes are then listed together as **Unclassified user processes**
instead of calling them all background. Until the first list arrives, the
header reads **Apps (detecting…)**. Kernel processes are still shown
separately. bex never uses XWayland's window list on a Wayland
desktop, because that only knows about X11 apps.

How the window list is fetched:

- On a background thread, never on the UI thread, with one request at a
  time. A request gets 400 ms in total, a reply at most 2 MiB and 1024
  windows. Anything else is reported as the reason detection is unavailable,
  for example a malformed reply, a timeout, a missing socket or a socket
  served by another user.
- Every 1.5 s while the Processes page is visible and every 5 s otherwise,
  slowing down to every 10 s after repeated failures. A list stays valid for
  about two request intervals; after that the Apps group says it is out of
  date instead of trusting it.
- Each window's PID is checked against `/proc` when the reply arrives: the
  process's start time is recorded with it, so a PID reused later cannot
  pick up the window. A process that started after the request was sent is
  not counted as an app.
- A list is used only when the owner of every window was identified. If a
  window has no usable process ID, or its process could not be read or was
  replaced while the list was read, the list cannot show which processes own
  no window. It then counts as a failed request: a complete list that is
  still valid is kept until it expires, and after that user processes are
  unclassified, with what was missing as the reason. A complete list with no
  windows at all is still an answer: Apps (0).
- If Hyprland's socket is busy (its connection queue is full), bex
  retries within the 400 ms and otherwise tries again at the next interval.
- Only process IDs are kept. Window titles, classes and commands are never
  stored or logged.
- Pause freezes the groups. On resume, replies to requests sent before the
  pause are dropped and a new list is fetched at once.

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
- **Quitting during an export**: an export still blocked when you quit (for
  example on an unresponsive network mount) cannot be cancelled. It is
  abandoned, and bex prints one line naming the destination to its
  standard error. The file at that name is either complete or unchanged. On a
  filesystem without unnamed temporary files, a `<name>.XXXXXX` temporary may
  be left beside it.

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
bex [--interval 0.5|1|2|5]
    [--page cpu|memory|disks|network|temperatures|processes]
    [--size WIDTHxHEIGHT]
    [--screenshot FILE.png [--screenshot-delay SECONDS]]
    [--quit-after SECONDS]
    [--theme-info]
    [--frame-stats]
```

- `--interval` replaces the saved interval at startup. Like a choice made in
  **View > Update Interval**, it is saved and used on the next start.
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

- bex runs as your normal user and never asks for or uses elevated
  privileges.
- It makes no network connections, has no telemetry and writes nothing except
  its settings file and the CSV files you explicitly export.
- It only reads files under `/proc` and `/sys`, and asks the kernel for
  filesystem capacity (`statvfs`) of local mounts. To tell apps apart it asks
  your own desktop session for its window list: Hyprland over its local IPC
  socket, or the local X server. It keeps only process IDs from the answer.
- The one thing it changes is a process you confirm End task for: it sends
  that one process of yours `SIGTERM` or `SIGKILL`, with your normal user's
  permissions. The kernel has the final say.
- It does not run subprocesses, tune the system or generate load.
- Everything it shows is what any user on the machine can already read.
  - Some per-process fields, such as disk I/O counters, are restricted by the
    kernel for other users' processes. Those show as unavailable.
  - Hardened kernels (for example with `hidepid`) show fewer processes.

## Limitations

- Linux only, by design.
- Per-process I/O counters are unavailable for other users' processes without
  privileges. bex does not try to work around this.
- **End task**:
  - Needs pidfds (Linux 5.3 or later) and a `/proc` mounted for bex's
    own PID namespace. Otherwise nothing is sent and a message says why.
  - Ends your own processes only. There is no way to end another user's or a
    system process, by design.
  - It is offered only for the live list bex reads from this system's
    `/proc`. With recorded or synthetic process lists (as in the tests), the
    IDs may not name this system's processes, so the controls stay disabled.
- **Disk throughput**:
  - Throughput is per whole block device. Per-partition and per-filesystem
    I/O rates are not shown.
  - Stacked devices (LVM, dm-crypt, md RAID) can be selected individually but
    are excluded from the "all physical disks" total.
- Filesystem capacity is not shown for network, FUSE or other remote
  filesystems.
- Sensor names come from the kernel driver. Some chips report only generic
  labels such as `temp1`.
- App detection covers Hyprland and X11 only. On other Wayland desktops all
  user processes are shown as unclassified. Between a window closing and the
  next list, Hyprland may still name a process that has just exited; if its
  PID were reused within that interval, the newcomer is excluded because it
  started after the request.
- The X11 path is built and unit-tested, and was run against a private Xvfb
  server with test windows, but not on a live X11 desktop with a real window
  manager (the development machine runs Hyprland). If the X server is already
  unresponsive when bex first connects, `xcb_connect()` can block the
  app-detection thread (never the window); after 5 s the group reads "Apps
  (unavailable)" and user processes stay unclassified. On quit that thread is
  abandoned after a bounded wait.
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
    The CSV file itself is written on a worker thread, and so is a confirmed
    End task (its checks read `/proc`).
  - When the window closes, bex waits up to two seconds in total for
    its worker threads: first the collector, which gives its capacity-check
    thread 200 ms, then the window-list thread, then any running export and
    any End task request, for at most one second each of what remains. A
    confirmation still open is closed without sending anything. A worker still stuck in a blocked kernel call after that is
    abandoned instead of waited for.
  - This limits bex's own waiting, not the process exit. Saving
    settings is synchronous, and the kernel may delay the exit of a process
    whose thread is stuck in uninterruptible I/O.
  - If the system refuses to create a worker thread (the per-user process
    limit or the cgroup's task limit is reached), nothing crashes or waits
    forever. The status line says the collector could not start, app
    detection says the window-list thread could not start, and filesystems
    show "capacity check could not start". Each is retried on its own,
    after 1 s and then backing off to 30 s; the collector and window-list
    threads are also retried at once on resume.
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
  name, and graphs expose their current values to screen readers. That
  includes each per-core tile on the CPU page ("CPU 3 42.0 %", or why no
  value is shown); the tiles are not Tab stops. When samples have stopped or
  collection is paused, a graph or tile says so before its last value
  ("Paused, last CPU 3 42.0 %"); a graph with a selected sample reads that
  sample and its age instead. These descriptions are
  updated when a sample arrives or the selection changes, not at the
  animation frame rate.
- **Review**: [docs/QUALITY_REVIEW.md](docs/QUALITY_REVIEW.md) records the
  latest code review: ratings per area, each finding and its outcome, and
  what was not verified.
