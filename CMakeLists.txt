cmake_minimum_required(VERSION 3.7)
project(oicb C)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  # libedit (or whatever) from base system doesn't satisfy us
  set(Readline_PREFER_GNU Yes)
endif()

find_package(Curses REQUIRED)
if (APPLE)
set(CMAKE_FIND_DEBUG_MODE True)
find_package(Readline REQUIRED)
set(CMAKE_FIND_DEBUG_MODE False)
endif()

add_executable(${CMAKE_PROJECT_NAME}
	chat.c
	history.c
	oicb.c
	private.c
	utf8.c
	)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
	${CURSES_INCLUDE_DIRS}
	${Readline_INCLUDE_DIRS}
	)
target_link_libraries(${CMAKE_PROJECT_NAME}
	${CURSES_LIBRARIES}
	${Readline_LIBRARIES}
	)
install(TARGETS ${CMAKE_PROJECT_NAME} DESTINATION bin) 

if (APPLE OR CMAKE_SYSTEM_NAME MATCHES ".*BSD.*")
	message(STATUS "It looks you're running BSD system and do not need libbsd")
else()
	find_package(BSD REQUIRED)
	list(APPEND BSD_DEFINITIONS -DLIBBSD_OVERLAY)
	list(APPEND BSD_DEFINITIONS -D_BSD_SOURCE=1 -D_GNU_SOURCE=1 -D_DEFAULT_SOURCE=1 -D_XOPEN_SOURCE=700)

	# libbsd warns about OpenBSD vs. NetBSD strnvis incompatibility
	list(APPEND BSD_DEFINITIONS -Wno-error=cpp)

	add_definitions(${BSD_DEFINITIONS})
	target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${BSD_INCLUDE_DIRS})
	target_link_libraries(${CMAKE_PROJECT_NAME} ${BSD_LIBRARIES})
	set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
		COMPILE_OPTIONS "-include${CMAKE_CURRENT_SOURCE_DIR}/compat.h"
		)
endif()

include(CheckSymbolExists)
include(CMakePushCheckState)

check_symbol_exists(pledge unistd.h HAVE_PLEDGE)
if (HAVE_PLEDGE)
	add_definitions(-DHAVE_PLEDGE)
endif()
check_symbol_exists(unveil unistd.h HAVE_UNVEIL)
if (HAVE_UNVEIL)
	add_definitions(-DHAVE_UNVEIL)
endif()

cmake_push_check_state()
list(APPEND CMAKE_REQUIRED_INCLUDES ${Readline_INCLUDE_DIRS})
list(APPEND CMAKE_REQUIRED_LIBRARIES ${Readline_LIBRARIES})
check_symbol_exists(rl_bind_keyseq "stdio.h;readline/readline.h" HAVE_RL_BIND_KEYSEQ)
check_symbol_exists(rl_mark "stdio.h;readline/readline.h" HAVE_RL_MARK)
cmake_pop_check_state()
if (NOT HAVE_RL_MARK)
	message(SEND_ERROR "unusable readline detected, separate installation of GNU readline may help")
endif()
if (HAVE_RL_BIND_KEYSEQ)
	add_definitions(-DHAVE_RL_BIND_KEYSEQ)
endif()

if (CMAKE_C_COMPILER_ID STREQUAL Clang OR CMAKE_C_COMPILER_ID STREQUAL GNU)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wshadow -Wpointer-arith -Wcast-qual -Wsign-compare")

	set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Werror")
endif()

# man handling
set(MANDIR "${CMAKE_INSTALL_PREFIX}/man" CACHE PATH "Prefix to directory where to install manual page")
message(STATUS "Manual page will be installed under ${MANDIR}")

install(FILES ${CMAKE_PROJECT_NAME}.1 DESTINATION "${MANDIR}/man1")
