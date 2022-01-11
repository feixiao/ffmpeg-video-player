##
# Find SDL2 Libraries.
#
# Once done this will define
#   SDL2_FOUND        - System has the all required components.
#   SDL2_INCLUDE_DIRS - Include directory necessary for using the required components headers.
#   SDL2_LIBRARIES    - Link these to use the required ffmpeg components.
##

# find_library(SDL2_LIBRARIES SDL2)

PKG_SEARCH_MODULE(SDL2 REQUIRED sdl2)
if (CMAKE_SYSTEM_NAME MATCHES "Darwin")
    set(SDL2_LIBRARIES /usr/local/Cellar/sdl2/2.0.12_1/lib/libSDL2.dylib)
endif ()
