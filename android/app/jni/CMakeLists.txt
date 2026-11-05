# The native part of the APK: SDL2 from source, then the game (the repo's own
# CMakeLists.txt) as libmain.so, which SDL's activity loads (ADR 0010).
cmake_minimum_required(VERSION 3.16)
# Optimised in every variant: a debug APK differs in its Java side only.
# Gradle's debug variant asks for Debug, which built the runtime (renderer,
# kernel, audio) at -O0 -- far too slow on a Switch's CPU.
set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
project(mcdub3_android C CXX)

# SDL2, shared: libSDL2.so, loaded by SDLActivity before libmain.so.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${MC3_SDL_DIR} SDL2)
if(NOT TARGET SDL2::SDL2)
    add_library(SDL2::SDL2 ALIAS SDL2)
endif()
# find_package(SDL2) in the toolkit: the target above already is SDL2.
file(WRITE ${CMAKE_BINARY_DIR}/sdl2cfg/SDL2Config.cmake "set(SDL2_FOUND TRUE)\n")
set(SDL2_DIR ${CMAKE_BINARY_DIR}/sdl2cfg CACHE PATH "" FORCE)

add_subdirectory(${MC3_REPO} mcdub3)
