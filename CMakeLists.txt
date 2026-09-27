cmake_minimum_required(VERSION 3.21)

project(PulseScope
    VERSION 1.0.0
    DESCRIPTION "Native Linux performance monitor"
    LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "PulseScope reads Linux /proc and /sys interfaces and only supports Linux "
        "(detected: ${CMAKE_SYSTEM_NAME}).")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PULSESCOPE_BUILD_TESTS "Build the Qt Test suites" ON)
option(PULSESCOPE_WERROR "Treat compiler warnings as errors" OFF)
set(PULSESCOPE_SANITIZE "" CACHE STRING "Semicolon separated sanitizers, e.g. address;undefined")

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Threads REQUIRED)
qt_standard_project_setup()
include(GNUInstallDirs)

set(PULSESCOPE_APP_ID "io.github.kidusm001.PulseScope")

# ---------------------------------------------------------------------------
# Shared compiler settings
add_library(pulsescope_flags INTERFACE)
target_compile_options(pulsescope_flags INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
    -Wcast-align -Wformat=2 -Wimplicit-fallthrough -Wnull-dereference -Wundef)
if(PULSESCOPE_WERROR)
    target_compile_options(pulsescope_flags INTERFACE -Werror)
endif()
if(PULSESCOPE_SANITIZE)
    list(JOIN PULSESCOPE_SANITIZE "," _ps_sanitizers)
    target_compile_options(pulsescope_flags INTERFACE -fsanitize=${_ps_sanitizers} -fno-omit-frame-pointer)
    target_link_options(pulsescope_flags INTERFACE -fsanitize=${_ps_sanitizers})
endif()

# ---------------------------------------------------------------------------
# Core: kernel parsing, delta math, sampling and bounded history. No Qt.
add_library(pulsescope_core STATIC
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
target_include_directories(pulsescope_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pulsescope_core PUBLIC pulsescope_flags Threads::Threads)
set_target_properties(pulsescope_core PROPERTIES AUTOMOC OFF)

# ---------------------------------------------------------------------------
# UI library: collector thread, models, charts and the main window.
add_library(pulsescope_ui STATIC
    src/app/appicon.cpp
    src/app/appicon.h
    src/app/cli.cpp
    src/app/cli.h
    src/app/collector.cpp
    src/app/collector.h
    src/app/csvexporter.cpp
    src/app/csvexporter.h
    src/app/mainwindow.cpp
    src/app/mainwindow.h
    src/app/metricbutton.cpp
    src/app/metricbutton.h
    src/app/processmodel.cpp
    src/app/processmodel.h
    src/app/timechart.cpp
    src/app/timechart.h)
target_link_libraries(pulsescope_ui PUBLIC pulsescope_core Qt6::Widgets)
target_compile_definitions(pulsescope_ui PUBLIC
    PULSESCOPE_VERSION="${PROJECT_VERSION}"
    PULSESCOPE_APP_ID="${PULSESCOPE_APP_ID}"
    QT_NO_KEYWORDS)

qt_add_executable(pulsescope src/app/main.cpp)
target_link_libraries(pulsescope PRIVATE pulsescope_ui)

# ---------------------------------------------------------------------------
# Installation (user-local: cmake --install build --prefix ~/.local)
install(TARGETS pulsescope RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/${PULSESCOPE_APP_ID}.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES data/${PULSESCOPE_APP_ID}.metainfo.xml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
# The desktop entry gets an absolute Exec path computed at install time, so it
# follows `--prefix` and works for ~/.local even when ~/.local/bin is not on
# PATH. cmake/desktopentry.cmake resolves absolute or relative bindirs and
# applies the Desktop Entry Exec escaping rules.
set(_ps_desktop_in "${CMAKE_CURRENT_SOURCE_DIR}/data/${PULSESCOPE_APP_ID}.desktop.in")
set(_ps_desktop_out "${CMAKE_CURRENT_BINARY_DIR}/${PULSESCOPE_APP_ID}.desktop")
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
if(PULSESCOPE_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    set(PULSESCOPE_ARTIFACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/artifacts")

    function(pulsescope_add_test name)
        qt_add_executable(${name} tests/${name}.cpp ${ARGN})
        target_link_libraries(${name} PRIVATE pulsescope_ui Qt6::Test)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PULSESCOPE_ARTIFACT_DIR=${PULSESCOPE_ARTIFACT_DIR};QT_LOGGING_RULES=qt.qpa.*=false"
            TIMEOUT 120)
    endfunction()

    # Core regression suite (parsers, metrics, history, sampler, collector,
    # chart rendering), written and maintained by independent QA.
    pulsescope_add_test(tst_adversarial)
    # Focused cases for identity, escaping and overflow guarantees.
    pulsescope_add_test(tst_core)
    # Read-only checks against this host's real /proc and /sys.
    pulsescope_add_test(tst_live)
    # Main window, process model and export, offscreen.
    pulsescope_add_test(tst_ui)
    # Graph animation, area fills, Details view and theme, written by
    # independent QA.
    pulsescope_add_test(tst_graph_upgrade)
    add_test(NAME desktop_entry
        COMMAND ${CMAKE_COMMAND} -DPS_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DPS_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/desktopentry_test.cmake)
    set_tests_properties(tst_live PROPERTIES LABELS "live")
    set_tests_properties(tst_ui PROPERTIES LABELS "ui")
    # Runs its frame-timing cases again at several device pixel ratios in
    # child processes, several minutes in all.
    set_tests_properties(tst_graph_upgrade PROPERTIES LABELS "ui" TIMEOUT 1800)
endif()
