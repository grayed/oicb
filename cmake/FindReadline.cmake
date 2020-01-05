# - Try to find Readline include dirs and libraries 
#
# Variables defined by this module:
#
#  Readline_FOUND            All readline library components were found.
#  Readline_INCLUDE_DIRS     The include directories needed to use readline.
#  Readline_LIBRARIES        The libraries needed to use readline.

find_path(Readline_INCLUDE_DIR
    NAMES readline/readline.h ereadline/readline.h
)
set(Readline_INCLUDE_DIRS ${Readline_INCLUDE_DIR})

find_library(Readline_LIBRARY
    NAMES readline ereadline
)
set(Readline_LIBRARIES ${Readline_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Readline DEFAULT_MSG
    Readline_LIBRARY Readline_INCLUDE_DIR)

mark_as_advanced(Readline_INCLUDE_DIR Readline_LIBRARY)
