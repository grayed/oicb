cmake_minimum_required(VERSION 3.3)
project(oicb C)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

find_package(Curses REQUIRED)
find_package(Readline REQUIRED)

add_executable(${CMAKE_PROJECT_NAME} oicb.c)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
	${CURSES_INCLUDE_DIRS}
	${Readline_INCLUDE_DIRS}
	)
target_link_libraries(${CMAKE_PROJECT_NAME}
	${CURSES_LIBRARIES}
	${Readline_LIBRARIES}
	)
install(TARGETS ${CMAKE_PROJECT_NAME} DESTINATION bin) 
