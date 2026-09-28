# Quality review

One review of PulseScope: what was examined, the findings and their
outcome, ratings per area, and what was not verified. It is not a
certification, an accessibility conformance claim or an overall grade.

## Scope

- Commit `30fd20ee` (smooth filled graphs, native Qt theming), plus
  grouping processes into Apps, Background processes and Kernel processes
  (`src/app/windowowners.*`, the grouped model and view) and the repairs
  below.
- Examined: `src/core`, `src/app`, all tests, `CMakeLists.txt`, the desktop
  entry and metainfo data, and the README.

## Who took part

| Role | Model | Reasoning effort | Work |
| --- | --- | --- | --- |
| Implementation and author tests | Claude Opus 5.5 | xhigh | All production code, `tst_ui`, `tst_core`, CMake and README changes |
| Independent QA | Claude Opus 5.5 | high | Owns `tst_process_groups`, `tst_council`, `tst_adversarial` and `tst_graph_upgrade`; ran them in normal and sanitizer builds against pinned source hashes |
| Planning, coordination, architecture review | GPT-6 Astra | high or above | Domain and data invariants, arithmetic, parsing, module interfaces, history and identity |
| Systems review | Claude Opus 5.5 | high | Ownership, threads and IPC, resource bounds, tests, build and install |
| Native experience review | Claude Fable 5.1 | high | Model/view use, hierarchy, keyboard and accessibility, theme and scale, states, documentation |

The three reviewers read source, tests and recorded evidence. They did not
edit code or run builds or tests; test runs came from the author and QA.

## Method and scale

One independent round, one discussion of material disagreements, then
targeted verification of each accepted fix. The review did not loop to reach
higher ratings.

Findings have a severity (P0 blocks operation or risks data loss, P1 major
ordinary-use defect, P2 bounded correctness or usability issue, P3 polish)
and a confidence from 0 to 100: above 80 is confirmed, 60 to 79 is only an
investigation target.

Areas are rated from 0 to 4:

| Score | Meaning |
| --- | --- |
| 0 | Broken or unusable in an evidenced ordinary scenario |
| 1 | Major correctness, reliability or usability gaps |
| 2 | Functional with material evidenced gaps or weak verification |
| 3 | Solid behaviour with bounded improvements or documented limitations |
| 4 | Strong verified behaviour for the stated target and scope |
| NA | Not inspected, or not meaningfully applicable |

A passing test count is not a coverage percentage, and a screenshot does not
prove screen-reader use or frame cadence.

## Ratings

Scores are per area and per reviewer; they are not averaged. Confidence is
the reviewer's own.

Architecture (GPT-6 Astra):

| Area | Before → after | Confidence after | Primary evidence |
| --- | --- | --- | --- |
| Numerical and data correctness | 2 → 3 | 96 | ARCH-001: process CPU and I/O rates compare their own acquisition windows |
| Input and error contracts | 2 → 3 | 94 | ARCH-003: read-cap overflow is an explicit failure; ARCH-002: metadata failures clear and retry |
| Resource and data bounds | 3 → 3 | 91 | Bounded histories and quotas; retained read payload bounded to the cap |
| Module depth and locality | 3 → 3 | 94 | Repairs stayed inside the sampler behind its existing interface |
| Testability and coverage quality | 3 → 3 | 93 | Exact boundary, timing-skew and metadata fixtures on existing seams |

Systems (Claude Opus 5.5):

| Area | Before → after | Confidence after | Primary evidence |
| --- | --- | --- | --- |
| Ownership and lifecycle | 3 → 4 | 75 | Unstarted workers are cleaned up at all three thread boundaries |
| Threading, IPC and recovery | 3 → 4 | 75 | One generation-stamped request in flight; bounded thread-start retry; deadline-bounded compositor client |
| Performance and resource bounds | 3 → 3 | 70 | One snapshot, capacity batch, window reply and export in flight; 2 MiB and 1024-window reply caps |
| Build, install and portability | 3 → 3 | 80 | Target-scoped dependencies, validated optional X11, fatal sanitizers; Qt 6.4 floor untested |
| Testing and verification | 3 → 4 | 70 | Honest labels, sanitizer time budgets, pinned app binary, compile-time full-UI guard |

Native experience (Claude Fable 5.1):

| Area | Before → after | Confidence before → after | Primary evidence |
| --- | --- | --- | --- |
| Hierarchy and progressive disclosure | 3 → 3 | 85 → 80 | Simple default with Details; grouped processes with counts |
| Keyboard, focus and accessibility | 3 → 3 | 75 → 80 | Per-core values and stale notices reach `QAccessible`; no screen reader run |
| Theme, colour and legibility | 3 → 4 | 70 → 70 | The user's Qt theme; near-full filesystems warn in text as well as colour |
| Responsive layout, scaling and fonts | 4 → 4 | 70 → 85 | Resolved widget `QFontInfo` matches the configured font |
| Truthful empty, stale, warm-up and error states | 4 → 4 | 80 → 85 | Unavailable, paused and expired states are shown, never zeros |
| Process table and chart interaction | 3 → 4 | 80 → 80 | Name takes the spare width in Simple; native tree keys documented |
| Documentation and terminology | 3 → 4 | 85 → 80 | README matches the grouped view and accessibility behaviour |

## Findings

Fixed, with a regression test where the behaviour is testable here:

| ID | Severity | Issue | Verification |
| --- | --- | --- | --- |
| PG-QA-001 | P2 | Resume kept using an expired window-owner list | `tst_process_groups expiredListNotReusedAfterResume`, `tst_ui processGroupsExpireAndExplain` |
| SYS-008 | P3 | A full Hyprland socket backlog was reported as "served by another user" | `tst_process_groups hyprlandFullBacklogIsBusyNotAnotherUser` |
| SYS-009 | P3 | The X11 XRes version query was an unbounded round trip | XRes discovery waits under the request deadline and absent XRes is reported as unsupported; source review only, no X server run |
| SYS-010 | P3 | An incomplete window list could mark owners as Background | `tst_process_groups` omission counters, `tst_ui processGroupsNeedCompleteWindowLists` |
| SYS-011 | P3 | `PULSESCOPE_X11` accepted any value | CMake accepts AUTO, ON or OFF in any case and stops on anything else |
| SYS-001 | P2 | Thread creation failure escaped or leaked at worker start | `tst_council threadStartFailure`, in a child process with only its own process limit lowered |
| ARCH-001 | P2 | A delayed process-only read inflated CPU and I/O rates | `tst_council processOnlyDelayDoesNotInventRates` |
| ARCH-002 | P2 | UID and command metadata could stay outdated, or failed, forever | `tst_council sameIdentityMetadataRefreshes`, `initialMetadataFailureRecovers` |
| ARCH-003 | P2 | A capped read could be parsed as a complete table | `tst_council readCapIsNotACompleteTable` |
| UX-001 | P2 | Per-core tiles exposed no accessible value, and later old values sounded current | `tst_council compactChart*`, `detailsCoreChartsStayHonest`; `tst_ui chartWithoutSamplesInWindow` |
| UX-002 | P2 | Simple mode gave the spare width to the memory column | `tst_ui processGroupsKeepViewState` |
| UX-003 | P3 | The near-full filesystem bar used a non-warning hue | `tst_ui nearlyFullFilesystemWarns` |
| UX-201 | P3 | Native tree keys were undocumented | README keyboard table and Process groups section |
| SYS-002 | P3 | An export abandoned at close was silent | A warning at shutdown and a README note; `tst_ui csvExportShutdownIsBounded` |
| SYS-003 | P3 | UBSan reports did not fail tests | `-fno-sanitize-recover=all`; a small UB probe exits 1 with GCC and Clang |
| SYS-004 | P3 | Sanitized graph runs exceeded their time limit | Larger CTest and Qt Test limits for sanitizer builds only |
| SYS-005 | P3 | Labels did not separate host-dependent and slow tests | Per-binary labels, documented in the README |
| SYS-006 | P3 | Required UI coverage could be compiled away, and the graph suite could use a stale app | App dependency and path, a skip-to-fail match and a compile-time guard |

Deferred on purpose. These are decisions, not defects waiting to be fixed:

- SYS-007: Qt's default `$ORIGIN` RUNPATH is a deployment option, not a
  reproduced failure.
- SYS-INV-02: an abandoned thread racing static destruction at exit is
  unproven; exit semantics stay unchanged without a reproducer.
- P-1: Smooth graphs already has a keyboard route through the View menu.
- clang-tidy's `History` divide-by-zero note: capacity is always 2 or more.
- UX-202 and UX-203: small wording and column-alignment polish.

Investigated:

- UX-101 (configured font absent from screenshots): closed; the resolved
  widget `QFontInfo` in the same environment matches the theme report.
- SYS-INV-01 (declared Qt 6.4 minimum): unknown. Only Qt 6.11.2 was built,
  no container runtime was available, and the minimum was not raised just
  because an older build was not run.

## Coverage by module

No line or branch coverage was measured. This lists which suites exercise
each module:

| Module | Exercised by |
| --- | --- |
| `src/core` parsers, delta math, history | `tst_adversarial`, `tst_core`, `tst_live` |
| `src/core/sampler` | `tst_adversarial`, `tst_council`, `tst_live` |
| `collector` | `tst_adversarial`, `tst_council`, `tst_ui` |
| `windowowners` (Hyprland client, X11 reply handling, window probe) | `tst_process_groups`, `tst_council` |
| `processmodel` and the grouped tree | `tst_process_groups` (with `QAbstractItemModelTester`), `tst_ui` |
| `timechart`, `metricbutton` | `tst_graph_upgrade`, `tst_ui`, `tst_council` |
| `mainwindow` | `tst_ui`, `tst_process_groups`, `tst_council` |
| `csvexporter` | `tst_ui` |
| `cli` and theme selection | `tst_graph_upgrade`, `tst_ui` |
| Desktop entry | `desktop_entry` |

## Test results

- Debug build with `-DPULSESCOPE_WERROR=ON`: all 8 CTest suites pass in
  about 400 s. Test functions, counting data rows: `tst_adversarial` 361,
  `tst_core` 13, `tst_ui` 58, `tst_graph_upgrade` 65, `tst_process_groups`
  73, `tst_council` 19, plus `tst_live` and `desktop_entry`.
- Address, undefined-behaviour and leak sanitizers: the full
  `tst_graph_upgrade` suite passes 65 of 65 in 886 s with no reports. Other
  suites ran sanitized for the code each change touched, not as a full
  matrix after every change.
- QA ran its own suites independently, normal and sanitized, against pinned
  source hashes. Clang and no-X11 builds pass the grouping and UI suites.
- The locally installed clang-tidy 22.1.8 ran 49 focused checks over the five
  core `.cpp` files. Its one finding, a divide-by-zero note in `History`, was
  a false positive (see Findings); positive controls confirmed the checks
  fire.

## Guidance used

External guidance was read and applied as reference material. No skill
package, plugin, MCP server or executable was installed, no downloaded skill
linter or external hosted review workflow was run, and no source, commands
or window titles were sent to outside services. Nothing here copies
third-party code or skill text.

Two skills already available locally were also applied: Impeccable's
product and audit guidance (native hierarchy, semantic data colours, the
system theme, keyboard focus, clear states, accessibility), translated to Qt
Widgets and `QAccessible`, and Codebase Design (module depth, ownership,
small interfaces, testable seams), without speculative adapters or rewrites.

Install counts were observed in September 2026, when the guidance was
assembled. They are trust signals for that date, not guarantees.

| Source | Pinned reference | License | Observed installs |
| --- | --- | --- | --- |
| ECC C++ Coding Standards | [affaan-m/ECC `e482e57`](https://github.com/affaan-m/ECC/blob/e482e579415fde18357cafce70f177ae19fd7f03/skills/cpp-coding-standards/SKILL.md) | MIT | 10,692 |
| ECC C++ Testing | [affaan-m/ECC `e482e57`](https://github.com/affaan-m/ECC/blob/e482e579415fde18357cafce70f177ae19fd7f03/skills/cpp-testing/SKILL.md) | MIT | 9,768 |
| Qt C++ Review | [TheQtCompanyRnD/agent-skills `71d6c10`](https://github.com/TheQtCompanyRnD/agent-skills/blob/71d6c10da78b9a764468ae11c86ab3bc4ca4921f/skills/qt-cpp-review/SKILL.md) | BSD-3-Clause or Qt commercial | 789 |
| Qt UI Design | [TheQtCompanyRnD/agent-skills `71d6c10`](https://github.com/TheQtCompanyRnD/agent-skills/blob/71d6c10da78b9a764468ae11c86ab3bc4ca4921f/skills/qt-ui-design/SKILL.md) | BSD-3-Clause or Qt commercial | 831 |
| Qt CMake Project | [TheQtCompanyRnD/agent-skills `71d6c10`](https://github.com/TheQtCompanyRnD/agent-skills/blob/71d6c10da78b9a764468ae11c86ab3bc4ca4921f/skills/qt-cmake-project/SKILL.md) | BSD-3-Clause or Qt commercial | 360 |

The C++ Core Guidelines and Qt's own documentation (object trees, threads and
QObjects, `QAbstractItemModel`, `QAbstractItemModelTester`, `QAccessible`,
the CMake manual) took precedence over any skill heuristic.

Adapted to a native Qt Widgets desktop app:

- Qt parent ownership counts as RAII; established Qt naming and low-level
  error-code interfaces stay.
- Tests stay on Qt Test and CTest, without GoogleTest, Catch2, downloaded
  dependencies or fuzzing tools.
- Widgets, not QML. The user's Kvantum palette and system fonts win over
  fixed font sizes, narrow reference layouts or touch-target dimensions.
- The Qt-free core stays Qt-free, and the Qt minimum was not raised.

Excluded: the lock-free stack example in a general C++ skill (Jeffallan C++
Pro) has no safe memory reclamation, so it was not used as a pattern. A
specialist C review workflow needed an unavailable runtime and was not run.

## Known limitations

- Tested with Qt 6.11.2 only; the declared Qt 6.4 minimum is unverified.
- X11 window detection compiles but was not run against an X server.
- Accessibility was checked through `QAccessible`; no screen reader such as
  Orca was run.
- Closing waits a bounded time for workers, but the kernel may still delay
  the exit of a thread stuck in uninterruptible I/O, and settings are saved
  synchronously.
- A process's user and command are re-read every 5 s, first 5 to 10 s after
  it appears, so a change can take that long to show.
- Pause freezes the groups; on resume, older replies are dropped and a new
  window list is fetched. An incomplete or unavailable list leaves processes
  unclassified rather than guessed to be background.

## Practices for future changes

- Own resources with RAII; Qt parent ownership is the form for QObjects.
- Keep sampling independent of rendering: frames never sample, sort or
  update accessible text.
- Report errors precisely, bound every read, reply and queue, use monotonic
  time, and identify processes by PID plus start time.
- Add a focused regression before a repair, and run it in normal and
  sanitizer builds.
- Test against the app's own windows and synthetic data; never log window
  titles or command lines.
- Keep logs, screenshots and binaries out of commits, under `build/`.
