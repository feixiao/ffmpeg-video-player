##
# CMake minimum required version for the project.
##
cmake_minimum_required(VERSION 3.11)

##
# player C Project CMakeLists.txt.
##
project(resampling C)

##
# Sets the C standard whose features are requested to build this target.
##
set(CMAKE_C_STANDARD 99)

##
# Adds player-sdl2.c executable target.
##
add_executable(resampling main.c)

##
# Adds include directories to be used when compiling and libraries to be used when
# linking target player-sdl.
##
target_include_directories(resampling PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(resampling PRIVATE ${FFMPEG_LIBRARIES} m)
