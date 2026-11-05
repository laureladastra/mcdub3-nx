cmake_minimum_required(VERSION 3.20)
project(nv2a_gl C)

# OpenGL 3.3 core renderer for the pushbuffer executor. SDL2 provides the
# window, the context and the entry points (gl_api.c resolves them itself),
# so there is no GL loader dependency and the same code builds for the
# Switch's Mesa.
add_library(xbox_nv2a_gl STATIC
    nv2a_gl.c
    gl_api.c
    gl_vsh.c
    gl_psh.c
    hd_pack.c
    gl_check.c
)
target_include_directories(xbox_nv2a_gl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# platform: host_main.c, the main thread for the window system (macOS).
target_link_libraries(xbox_nv2a_gl PUBLIC platform)

if(NINTENDO_SWITCH)
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_nv2a_gl PUBLIC SDL2::SDL2)
    # EGL (Mesa, portlibs): the compile thread's shared context (nv2a_gl.c).
    # SDL2's include path is portlibs/include/SDL2; EGL/ sits one level up.
    target_include_directories(xbox_nv2a_gl PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/include")
elseif(ANDROID)
    # SDL2 from source with the app (android/); GL ES through SDL's loader.
    target_link_libraries(xbox_nv2a_gl PUBLIC SDL2::SDL2)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    target_link_libraries(xbox_nv2a_gl PUBLIC PkgConfig::SDL2)
endif()
if(NOT MSVC)
    target_compile_options(xbox_nv2a_gl PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

# Key events go to the input module (input/sdl_keyboard.c, RECOMP_KEYBOARD).
if(NOT NINTENDO_SWITCH)
    target_link_libraries(xbox_nv2a_gl PUBLIC xbox_input)
endif()
