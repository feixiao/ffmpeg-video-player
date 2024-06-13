##
# CMake minimum required version for the project.
##
cmake_minimum_required(VERSION 3.11)

##
# FFmpeg-Video-Player C Project CMakeLists.txt.
##
project(FFmpeg_Video_Player C)

set(CMAKE_C_FLAGS "-std=c20")
##
# Include module pkg-config for CMake.
##
INCLUDE(FindPkgConfig)

##
# CMAKE_MODULE_PATH:FILEPATH=./FFmpeg-Video-Player/Modules
##
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/Modules)

##
# Find FFmpeg include directories and link libraries.
##
find_package(FFMPEG REQUIRED)
message ([STATUS] "FFMPEG_INCLUDE_DIRS" ${FFMPEG_INCLUDE_DIRS})
message ([STATUS] "FFMPEG_LIBRARIES" ${FFMPEG_LIBRARIES})


##
# Find SDL2 include directories and link libraries.
##
find_package(SDL2 REQUIRED)

# # Add log messages
message ([STATUS] "SDL2_INCLUDE_DIR" ${SDL2_INCLUDE_DIRS})
message ([STATUS] "SDL2_LIBRARYS" ${SDL2_LIBRARIES})


if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    add_definitions("-Wno-invalid-source-encoding")
    add_definitions("-O2")
elseif (CMAKE_SYSTEM_NAME MATCHES "Darwin")
    # set(CMAKE_OSX_SYSROOT "/Library/Developer/CommandLineTools/SDKs/MacOSX12.3.sdk")
    # set(CMAKE_CXX_FLAGS "--sysroot ${CMAKE_OSX_SYSROOT} ${CMAKE_CXX_FLAGS}")

    # https://stackoverflow.com/questions/69803659/what-is-the-proper-way-to-build-for-macos-x86-64-using-cmake-on-apple-m1-arm
    # set(CMAKE_OSX_ARCHITECTURES "x86_64")   

    set(CMAKE_OSX_ARCHITECTURES "arm64")
    add_definitions("-Wno-invalid-source-encoding")
    add_definitions("-fdata-sections -ffunction-sections")
    #    add_definitions("-flto")  # 影响clion 打断点
    #    add_definitions("-Os")

    include_directories("/opt/homebrew/include/")
else ()
    message(FATAL_ERROR "unknown CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}")
endif ()

##
# Add subdirectories to the build.
##
add_subdirectory(tutorial01)
add_subdirectory(tutorial02)
add_subdirectory(tutorial03)
add_subdirectory(tutorial04)
add_subdirectory(tutorial05)
add_subdirectory(tutorial06)
add_subdirectory(tutorial07)
add_subdirectory(tutorial08)
add_subdirectory(player)
add_subdirectory(resampling)
add_subdirectory(audio_encode)