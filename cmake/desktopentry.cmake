# Writes the desktop entry with an absolute, correctly escaped Exec path.
# Runs at install time (so `cmake --install --prefix` is honoured) and from the
# desktop_entry test. Inputs:
#   PS_DESKTOP_IN       template containing @PULSESCOPE_EXEC@
#   PS_DESKTOP_OUT      output file
#   PS_BINDIR           CMAKE_INSTALL_BINDIR (relative to the prefix, or absolute)
#   CMAKE_INSTALL_PREFIX
# DESTDIR is deliberately not part of Exec: it only stages the files.

foreach(_var PS_DESKTOP_IN PS_DESKTOP_OUT PS_BINDIR CMAKE_INSTALL_PREFIX)
    if(NOT DEFINED ${_var})
        message(FATAL_ERROR "desktopentry.cmake: ${_var} is not set")
    endif()
endforeach()

if(IS_ABSOLUTE "${PS_BINDIR}")
    set(_exec "${PS_BINDIR}/pulsescope")
else()
    set(_exec "${CMAKE_INSTALL_PREFIX}/${PS_BINDIR}/pulsescope")
endif()
string(REGEX REPLACE "/+" "/" _exec "${_exec}")

foreach(_code RANGE 1 31)
    string(ASCII ${_code} _ctrl)
    string(FIND "${_exec}" "${_ctrl}" _pos)
    if(_pos GREATER -1)
        message(FATAL_ERROR "Cannot write a desktop entry: the executable path contains control characters")
    endif()
endforeach()
string(ASCII 127 _del)
string(FIND "${_exec}" "${_del}" _pos)
if(_pos GREATER -1)
    message(FATAL_ERROR "Cannot write a desktop entry: the executable path contains control characters")
endif()

# Desktop Entry Specification, "The Exec key":
#  1. A literal '%' is written as "%%" (otherwise it is a field code).
#  2. The argument is double-quoted; inside quotes '"', '`', '$' and '\'
#     are escaped with a backslash.
#  3. Exec is a string value, so every backslash is escaped once more
#     (a literal backslash ends up as four, a dollar sign as "\\$").
string(REPLACE "%" "%%" _arg "${_exec}")
string(REPLACE "\\" "\\\\" _arg "${_arg}")
string(REPLACE "\"" "\\\"" _arg "${_arg}")
string(REPLACE "`" "\\`" _arg "${_arg}")
string(REPLACE "$" "\\$" _arg "${_arg}")
set(_arg "\"${_arg}\"")
string(REPLACE "\\" "\\\\" _arg "${_arg}")

file(READ "${PS_DESKTOP_IN}" _desktop)
string(REPLACE "@PULSESCOPE_EXEC@" "${_arg}" _desktop "${_desktop}")
file(WRITE "${PS_DESKTOP_OUT}" "${_desktop}")
