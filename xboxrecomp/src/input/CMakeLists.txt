# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
    sdl_keyboard.c      # Linux: the keyboard as player 1 (RECOMP_KEYBOARD)
    touch_pad.c         # touch screens: an on-screen pad (RECOMP_TOUCH_PAD)
)
if(NINTENDO_SWITCH)
    # The Switch's controllers, read through libnx and mapped by position.
    target_sources(xbox_input PRIVATE xinput_nx.c)
endif()

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32 AND NOT XBOXRECOMP_SDL_HOST)
    target_link_libraries(xbox_input PUBLIC platform xinput)
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
