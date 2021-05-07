# - Try to find Readline include dirs and libraries 
#
# Module input variables:
#
#  Readline_PREFER_GNU       Try to find real GNU readline first, off by default.
#
# Variables defined by this module:
#
#  Readline_FOUND            All readline library components were found.
#  Readline_INCLUDE_DIRS     The include directories needed to use readline.
#  Readline_LIBRARIES        The libraries needed to use readline.

set(Readline_PREFER_GNU NO CACHE BOOL "Prefer using the latest GNU readline we could find")

set(_Readline_PREFIX_HINTS)

if (Readline_PREFER_GNU)
  set(_Readline_TRY_HEADERS ereadline/readline.h readline/readline.h)
  set(_Readline_TRY_LIBS ereadline readline)

  find_program(_Readline_BREW_EXECUTABLE NAMES brew DOC "Homebrew executable")
  mark_as_advanced(_Readline_BREW_EXECUTABLE)
  
  if (_Readline_BREW_EXECUTABLE)
    execute_process(COMMAND ${BREW_EXECUTABLE} --prefix readline
                    OUTPUT VARIABLE _Readline_BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT VARIABLE _Readline_BREW_PREFIX_RESULT
                   )
    if (_Readline_BREW_PREFIX_RESULT EQUAL 0)
      set(_Readline_PREFIX_HINTS "${_Readline_BREW_PREFIX}")
    endif()
  endif()
else()
  set(_Readline_TRY_HEADERS readline/readline.h ereadline/readline.h)
  set(_Readline_TRY_LIBS readline ereadline)
endif()

find_path(Readline_INCLUDE_DIR
    NAMES ${_Readline_TRY_HEADERS}
    HINTS ${_Readline_PREFIX_HINTS}
)
set(Readline_INCLUDE_DIRS ${Readline_INCLUDE_DIR})

find_library(Readline_LIBRARY
    NAMES ${_Readline_TRY_LIBS}
    HINTS ${_Readline_PREFIX_HINTS}
)
set(Readline_LIBRARIES ${Readline_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Readline DEFAULT_MSG
    Readline_LIBRARY Readline_INCLUDE_DIR)

mark_as_advanced(Readline_INCLUDE_DIR Readline_LIBRARY)
