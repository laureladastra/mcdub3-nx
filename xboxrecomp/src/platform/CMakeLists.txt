cmake_minimum_required(VERSION 3.20)
project(platform C)

# Platform layer
#   xbox_winnt.h   - Win32 / NT type vocabulary (header only)
#   win32_compat.* - POSIX implementation of generic Win32 host primitives
#
# On Windows the real <windows.h> is used, so this is an INTERFACE library
# that only contributes the src/ include path. On Linux it is a real static
# library carrying the POSIX primitive implementations.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# src/, the directory this one lives in. Resolved relative to here rather than
# via CMAKE_SOURCE_DIR, which points at the *consuming* project's root when a
# game project pulls xboxrecomp in with add_subdirectory() -- the documented
# integration pattern -- and would send "platform/xbox_winnt.h" looking in the
# game's own src/ tree. PROJECT_SOURCE_DIR is no good either: this file
# declares its own project().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(WIN32 AND NOT XBOXRECOMP_SDL_HOST)
    add_library(platform INTERFACE)
    target_include_directories(platform INTERFACE ${XBOXRECOMP_SRC_DIR})
elseif(WIN32)
    # Windows with the SDL host: the real <windows.h>, plus host_main.c (the
    # renderer hands window calls to the main thread through it).
    add_library(platform STATIC host_main.c win32_host.c)
    target_include_directories(platform PUBLIC ${XBOXRECOMP_SRC_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    find_package(Threads REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PLATFORM_SDL2 REQUIRED IMPORTED_TARGET sdl2)
    target_link_libraries(platform PUBLIC Threads::Threads PkgConfig::PLATFORM_SDL2)
else()
    add_library(platform STATIC win32_compat.c host_main.c)
    target_include_directories(platform PUBLIC
        ${XBOXRECOMP_SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    find_package(Threads REQUIRED)
    target_link_libraries(platform PUBLIC Threads::Threads)
    target_compile_options(platform PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
    # macOS: host_main.c keeps the main thread for the window system (SDL).
    if(APPLE)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(PLATFORM_SDL2 REQUIRED IMPORTED_TARGET sdl2)
        target_link_libraries(platform PUBLIC PkgConfig::PLATFORM_SDL2)
    elseif(ANDROID)
        # SDL2 built from source with the app (android/): its target, as on
        # macOS host_main.c serves SDL from the thread that started it.
        target_link_libraries(platform PUBLIC SDL2::SDL2)
    endif()
endif()
