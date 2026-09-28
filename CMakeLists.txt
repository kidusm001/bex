cmake_minimum_required(VERSION 3.21)

project(bex
    VERSION 1.0.0
    DESCRIPTION "Native Linux performance monitor"
    LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "bex reads Linux /proc and /sys interfaces and only supports Linux "
        "(detected: ${CMAKE_SYSTEM_NAME}).")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BEX_BUILD_TESTS "Build the Qt Test suites" ON)
option(BEX_WERROR "Treat compiler warnings as errors" OFF)
set(BEX_SANITIZE "" CACHE STRING "Semicolon separated sanitizers, e.g. address;undefined")
set(BEX_X11 AUTO CACHE STRING "X11 window-owner detection for the Apps group: AUTO, ON or OFF")
set_property(CACHE BEX_X11 PROPERTY STRINGS AUTO ON OFF)
string(TOUPPER "${BEX_X11}" bex_x11_mode)
if(NOT bex_x11_mode MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "BEX_X11 must be AUTO, ON or OFF, not '${BEX_X11}'")
endif()
set(BEX_X11 "${bex_x11_mode}")

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Threads REQUIRED)
qt_standard_project_setup()
include(GNUInstallDirs)

set(BEX_APP_ID "io.github.kidusm001.bex")

# ---------------------------------------------------------------------------
# Shared compiler settings
add_library(bex_flags INTERFACE)
target_compile_options(bex_flags INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wcast-align -Wformat=2 -Wimplicit-fallthrough -Wnull-dereference -Wundef)
if(BEX_WERROR)
    target_compile_options(bex_flags INTERFACE -Werror)
endif()
if(BEX_SANITIZE)
    list(JOIN BEX_SANITIZE "," _ps_sanitizers)
    # No recovery: a sanitizer report ends the process with an error, so a
    # test run cannot pass while printing "runtime error:" lines.
    target_compile_options(bex_flags INTERFACE
        -fsanitize=${_ps_sanitizers} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(bex_flags INTERFACE -fsanitize=${_ps_sanitizers} -fno-sanitize-recover=all)
endif()

# ---------------------------------------------------------------------------
# Core: kernel parsing, delta math, sampling and bounded history. No Qt.
add_library(bex_core STATIC
    src/core/endtask.cpp
    src/core/endtask.h
    src/core/format.cpp
    src/core/format.h
    src/core/history.cpp
    src/core/history.h
    src/core/linuxparsers.cpp
    src/core/linuxparsers.h
    src/core/metrics.cpp
    src/core/metrics.h
    src/core/sampler.cpp
    src/core/sampler.h
    src/core/snapshot.h)
target_include_directories(bex_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(bex_core PUBLIC bex_flags Threads::Threads)
set_target_properties(bex_core PROPERTIES AUTOMOC OFF)

# ---------------------------------------------------------------------------
# UI library: collector thread, models, charts and the main window.
add_library(bex_ui STATIC
    src/app/appicon.cpp
    src/app/appicon.h
    src/app/cli.cpp
    src/app/cli.h
    src/app/collector.cpp
    src/app/collector.h
    src/app/csvexporter.cpp
    src/app/csvexporter.h
    src/app/endtaskrunner.cpp
    src/app/endtaskrunner.h
    src/app/mainwindow.cpp
    src/app/mainwindow.h
    src/app/metricbutton.cpp
    src/app/metricbutton.h
    src/app/processmodel.cpp
    src/app/processmodel.h
    src/app/settingsimport.cpp
    src/app/settingsimport.h
    src/app/timechart.cpp
    src/app/timechart.h
    src/app/windowowners.cpp
    src/app/windowowners.h)
target_link_libraries(bex_ui PUBLIC bex_core Qt6::Widgets)
target_compile_definitions(bex_ui PUBLIC
    BEX_VERSION="${PROJECT_VERSION}"
    BEX_APP_ID="${BEX_APP_ID}"
    QT_NO_KEYWORDS)

# Optional: on an X11 session, window owners come from the window manager's
# _NET_CLIENT_LIST and the X server's XRes client IDs (libxcb, libxcb-res).
# Hyprland support needs no extra library.
if(NOT BEX_X11 STREQUAL "OFF")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(BEX_XCB IMPORTED_TARGET xcb xcb-res)
    endif()
    if(BEX_XCB_FOUND)
        target_link_libraries(bex_ui PRIVATE PkgConfig::BEX_XCB)
        target_compile_definitions(bex_ui PRIVATE BEX_HAVE_X11)
        message(STATUS "bex: X11 window-owner detection enabled")
    elseif(BEX_X11 STREQUAL "ON")
        message(FATAL_ERROR "BEX_X11=ON needs pkg-config and the xcb and xcb-res development files")
    else()
        message(STATUS "bex: X11 window-owner detection disabled (xcb or xcb-res not found)")
    endif()
else()
    message(STATUS "bex: X11 window-owner detection disabled (BEX_X11=OFF)")
endif()

qt_add_executable(bex src/app/main.cpp)
target_link_libraries(bex PRIVATE bex_ui)

# ---------------------------------------------------------------------------
# Installation (user-local: cmake --install build --prefix ~/.local)
install(TARGETS bex RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/${BEX_APP_ID}.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES data/${BEX_APP_ID}.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
# The desktop entry gets an absolute Exec path computed at install time, so it
# follows `--prefix` and works for ~/.local even when ~/.local/bin is not on
# PATH. cmake/desktopentry.cmake resolves absolute or relative bindirs and
# applies the Desktop Entry Exec escaping rules.
set(_ps_desktop_in "${CMAKE_CURRENT_SOURCE_DIR}/data/${BEX_APP_ID}.desktop.in")
set(_ps_desktop_out "${CMAKE_CURRENT_BINARY_DIR}/${BEX_APP_ID}.desktop")
install(CODE "
    set(PS_DESKTOP_IN [==[${_ps_desktop_in}]==])
    set(PS_DESKTOP_OUT [==[${_ps_desktop_out}]==])
    set(PS_BINDIR [==[${CMAKE_INSTALL_BINDIR}]==])
    include([==[${CMAKE_CURRENT_SOURCE_DIR}/cmake/desktopentry.cmake]==])
")
install(FILES ${_ps_desktop_out} DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

configure_file(cmake/uninstall.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake @ONLY)
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    COMMENT "Removing files listed in install_manifest.txt")

# ---------------------------------------------------------------------------
# Tests
if(BEX_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    set(BEX_ARTIFACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")
    # The graph suite's fractional-scale children take several times longer
    # under sanitizers (one alone can pass Qt Test's default 300 s limit for
    # a test function, while the suite waits up to 1500 s for it); the other
    # suites stay well inside 120 s either way.
    if(BEX_SANITIZE)
        set(_ps_slow_test_timeout 3600)
        set(_ps_slow_function_timeout_ms 1800000)
    else()
        set(_ps_slow_test_timeout 1800)
    endif()

    function(bex_add_test name)
        qt_add_executable(${name} tests/${name}.cpp ${ARGN})
        target_link_libraries(${name} PRIVATE bex_ui Qt6::Test)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;BEX_ARTIFACT_DIR=${BEX_ARTIFACT_DIR};QT_LOGGING_RULES=qt.qpa.*=false"
            TIMEOUT 120)
    endfunction()

    # Labels, per test binary:
    #   unit     synthetic /proc and /sys trees, injected clocks, local test
    #            sockets and at most the test's own /proc entries
    #   ui       widgets, offscreen
    #   host     also depends on this machine or session (its mount table, the
    #            real /proc and /sys, process limits and privileges, the
    #            user's Qt theme files), so results can differ between machines
    #   live     only this host's real /proc and /sys
    #   slow     several minutes
    #   install  install-time files
    # `-L unit -LE host` is the deterministic subset.

    # Core regression suite (parsers, metrics, history, sampler, collector,
    # chart rendering), written and maintained by independent QA. The
    # official build has every widget header, so its widget and main-window
    # cases must not compile away into skips.
    bex_add_test(tst_adversarial)
    target_compile_definitions(tst_adversarial PRIVATE PS_ADV_REQUIRE_FULL_UI=1)
    set_tests_properties(tst_adversarial PROPERTIES LABELS "unit;host")
    # Focused cases for identity, escaping and overflow guarantees.
    bex_add_test(tst_core)
    set_tests_properties(tst_core PROPERTIES LABELS "unit;host")
    # Read-only checks against this host's real /proc and /sys.
    bex_add_test(tst_live)
    set_tests_properties(tst_live PROPERTIES LABELS "live;host")
    # Main window, grouped process model, window-owner probe and export,
    # offscreen.
    bex_add_test(tst_ui)
    set_tests_properties(tst_ui PROPERTIES LABELS "ui;host")
    # Graph animation, area fills, Details view and theme, written by
    # independent QA. It runs its frame-timing cases again at several device
    # pixel ratios in child processes, and starts the application built here
    # for its theme cases.
    bex_add_test(tst_graph_upgrade)
    add_dependencies(tst_graph_upgrade bex)
    set_property(TEST tst_graph_upgrade APPEND PROPERTY ENVIRONMENT "PS_GU_APP=$<TARGET_FILE:bex>")
    if(DEFINED _ps_slow_function_timeout_ms)
        set_property(TEST tst_graph_upgrade APPEND PROPERTY ENVIRONMENT
            "QTEST_FUNCTION_TIMEOUT=${_ps_slow_function_timeout_ms}")
    endif()
    set_tests_properties(tst_graph_upgrade PROPERTIES
        LABELS "ui;host;slow"
        TIMEOUT ${_ps_slow_test_timeout}
        FAIL_REGULAR_EXPRESSION "application binary not found")
    add_test(NAME desktop_entry
        COMMAND ${CMAKE_COMMAND} -DPS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DPS_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/desktopentry_test.cmake)
    set_tests_properties(desktop_entry PROPERTIES LABELS "unit;install")
    # Process groups, window-owner parsing, probe and grouped model, written
    # by independent QA.
    bex_add_test(tst_process_groups)
    set_tests_properties(tst_process_groups PROPERTIES LABELS "ui;unit")
    # Read-cap, process timing, metadata refresh and thread-start failure
    # (in child processes that lower their own RLIMIT_NPROC), written by
    # independent QA.
    bex_add_test(tst_council)
    set_tests_properties(tst_council PROPERTIES LABELS "unit;host")
    # End task: the pidfd backend against the test's own children, and the
    # Processes page controls with an injected backend (and once with the
    # real one, on its own child).
    bex_add_test(tst_endtask)
    set_tests_properties(tst_endtask PROPERTIES LABELS "ui;unit;host")
    # End task backend and controls, written by independent QA. It re-runs
    # itself in child modes (PS_ET_CHILD) for the processes it signals.
    bex_add_test(tst_end_task_adversarial)
    target_compile_definitions(tst_end_task_adversarial PRIVATE PS_ET_CORE=1 PS_ET_APP=1)
    set_tests_properties(tst_end_task_adversarial PROPERTIES LABELS "ui;unit;host")
    # One-time import of the settings saved under the former name
    # (PulseScope) and the app's identity, in a private configuration
    # folder; it also starts the application built here.
    bex_add_test(tst_settings_import)
    add_dependencies(tst_settings_import bex)
    set_property(TEST tst_settings_import APPEND PROPERTY ENVIRONMENT "BEX_APP=$<TARGET_FILE:bex>")
    set_tests_properties(tst_settings_import PROPERTIES LABELS "ui;unit;host")
    # Rename and settings import, written by independent QA. It starts the
    # application built here, each time with a private HOME; RN_STAGE (a
    # staged install) is optional.
    bex_add_test(tst_rename_adversarial)
    add_dependencies(tst_rename_adversarial bex)
    set_property(TEST tst_rename_adversarial APPEND PROPERTY ENVIRONMENT "RN_APP=$<TARGET_FILE:bex>")
    set_tests_properties(tst_rename_adversarial PROPERTIES LABELS "unit;host" TIMEOUT 180)
endif()
