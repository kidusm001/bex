# Checks the desktop entry Exec escaping and bindir resolution.
# Invoked by CTest: cmake -DPS_SOURCE_DIR=... -DPS_WORK_DIR=... -P this file.

function(check_exec prefix bindir expected)
    set(out "${PS_WORK_DIR}/desktop-test.desktop")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DPS_DESKTOP_IN=${PS_SOURCE_DIR}/data/io.github.kidusm001.PulseScope.desktop.in"
            "-DPS_DESKTOP_OUT=${out}"
            "-DPS_BINDIR=${bindir}"
            "-DCMAKE_INSTALL_PREFIX=${prefix}"
            -P "${PS_SOURCE_DIR}/cmake/desktopentry.cmake"
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "desktopentry.cmake failed for prefix '${prefix}'")
    endif()
    file(STRINGS "${out}" lines REGEX "^Exec=")
    if(NOT lines STREQUAL "Exec=${expected}")
        message(FATAL_ERROR "prefix '${prefix}' bindir '${bindir}':\n  got      '${lines}'\n  expected 'Exec=${expected}'")
    endif()
endfunction()

check_exec("/home/u/.local" "bin" [=["/home/u/.local/bin/pulsescope"]=])
check_exec("/usr" "/opt/tools/bin" [=["/opt/tools/bin/pulsescope"]=])
check_exec("/home/u/My Apps" "bin" [=["/home/u/My Apps/bin/pulsescope"]=])
check_exec("/tmp/100%/x" "bin" [=["/tmp/100%%/x/bin/pulsescope"]=])
check_exec("/tmp/a$b" "bin" [=["/tmp/a\\$b/bin/pulsescope"]=])
check_exec("/tmp/q\"uote" "bin" [=["/tmp/q\\"uote/bin/pulsescope"]=])
check_exec("/tmp/back\\slash" "bin" [=["/tmp/back\\\\slash/bin/pulsescope"]=])
check_exec("/tmp/tick`s" "bin" [=["/tmp/tick\\`s/bin/pulsescope"]=])
check_exec("/opt/ps/" "bin" [=["/opt/ps/bin/pulsescope"]=])
# Control characters cannot be represented safely and must fail loudly.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DPS_DESKTOP_IN=${PS_SOURCE_DIR}/data/io.github.kidusm001.PulseScope.desktop.in"
        "-DPS_DESKTOP_OUT=${PS_WORK_DIR}/desktop-test-bad.desktop"
        "-DPS_BINDIR=bin"
        "-DCMAKE_INSTALL_PREFIX=/tmp/line\nbreak"
        -P "${PS_SOURCE_DIR}/cmake/desktopentry.cmake"
    RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
if(rc EQUAL 0)
    message(FATAL_ERROR "a prefix containing a newline was accepted")
endif()
message(STATUS "desktop entry Exec escaping: all cases passed")
