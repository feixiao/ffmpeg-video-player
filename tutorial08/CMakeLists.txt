##
# CMake minimum required version for the project.
##
cmake_minimum_required(VERSION 3.11)



add_definitions("-Wall -O2")


##
# tutorial07 C Project CMakeLists.txt.
##
project(tutorial08 C)

##
# Sets the C standard whose features are requested to build this target.
##
set(CMAKE_C_STANDARD 99)

##
# Adds tutorial07.c executable target.
##
add_executable(tutorial08 tutorial08.c)

##
# Adds include directories to be used when compiling and libraries to be used when
# linking target tutorial07.
##
target_include_directories(tutorial08 PRIVATE ${FFMPEG_INCLUDE_DIRS} ${SDL2_INCLUDE_DIRS})
target_link_libraries(tutorial08 PRIVATE ${FFMPEG_LIBRARIES} ${SDL2_LIBRARIES} m)
