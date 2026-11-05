/*
 * sdl_keyboard.c -- the keyboard as player 1 on Linux (SDL), as the Windows
 * build already has it (xinput_device.c, RECOMP_KEYBOARD=1).
 *
 * The SDL renderers drained the window's events and dropped them, and
 * xbox_FramebufferKeyDown is a stub returning 0 outside Windows, so on Linux
 * RECOMP_KEYBOARD did nothing: a title waiting on PRESS START could only be
 * passed with a real controller (Midnight Club 3, 2026-10-16). Now the
 * renderers hand key events here and xbox_InputGetState reads them as pad 1.
 *
 * Laid out so the buttons are where their names say and the arrows drive
 * (2026-10-21; the WASD layout of 2026-10-17 put A/B on J/K, which made
 * menus confusing, and left the Switch's L/R unmapped). The d-pad -- songs
 * and map in an MC3 race -- moves to I J K L so the arrows can steer. The
 * Windows build keeps its Z X A S scheme (xinput_device.c):
 *
 *   A B X Y       A B X Y                 Enter / Backspace  START / BACK
 *   left / right  left stick (steer, menus)
 *   up            right trigger (gas) + left stick up (menus; not while
 *                 driving where the title says so: xbox_keyboard_driving)
 *   down          left trigger (brake) + left stick down (likewise)
 *   I J K L       d-pad up / left / down / right
 *   Q / E         white / black (the Switch's L / R; also Shift / Ctrl)
 *   Z / C         left / right thumb click
 *   T F G H or numpad 8 4 2 6           right stick (camera)
 *   Space         A (MC3's handbrake: the Switch's A)
 *   Tab           BACK too (select, reachable while driving); so is
 *                 Android's Back (the gesture, Esc on some keyboards)
 *
 * Keys arrive only while the game window has the focus; losing it releases
 * everything, so a key held while switching away does not stay down.
 * Written by the window thread, read by the USB thread, one byte each.
 */
/* Set by a title's own code (see keyboard_stick_y); defined on every
 * platform so the title links where there is no SDL keyboard. */
int (*xbox_keyboard_driving)(void);

#if (!defined(_WIN32) || defined(XBOXRECOMP_SDL_HOST)) && !defined(__SWITCH__)

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xinput_xbox.h"

static volatile unsigned char s_down[SDL_NUM_SCANCODES];

/* RECOMP_KEYBOARD_TRACE=1: each key as the pad layer gets it (a keyboard
 * that does nothing: do its keys arrive, and as what?). */
void xbox_sdl_keyboard_event(int scancode, int down)
{
    static int trace = -1;
    if (trace < 0) {
        const char *v = getenv("RECOMP_KEYBOARD_TRACE");
        trace = v && *v == '1';
    }
    if (trace && scancode >= 0 && scancode < SDL_NUM_SCANCODES
        && s_down[scancode] != (down ? 1 : 0))
        fprintf(stderr, "  [KEYBOARD] %s %s\n",
                SDL_GetScancodeName((SDL_Scancode)scancode), down ? "down" : "up");
    if (scancode >= 0 && scancode < SDL_NUM_SCANCODES)
        s_down[scancode] = down ? 1 : 0;
}

void xbox_sdl_keyboard_reset(void)
{
    memset((void *)s_down, 0, sizeof s_down);
}

int xbox_sdl_keyboard_down(int scancode)
{
    return scancode >= 0 && scancode < SDL_NUM_SCANCODES && s_down[scancode];
}

/* One window event, from the renderer's event loop (nv2a_gl, nv2a_vk). */
void xbox_sdl_keyboard_handle(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        xbox_sdl_keyboard_event(e->key.keysym.scancode, e->type == SDL_KEYDOWN);
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            xbox_sdl_keyboard_reset();
        break;
    default:
        break;
    }
}

int xbox_sdl_keyboard_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = getenv("RECOMP_KEYBOARD");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on;
}

/* Up/down move the left stick as well as the triggers, for menus -- but a
 * title may give stick up/down a job of its own while driving: the MC3
 * Remix raises and pulls in its race camera (2026-10-25), which the gas key
 * then did too. A title can say when the player is driving
 * (xbox_keyboard_driving, set by its own code); then up/down are the
 * triggers alone. RECOMP_KEYBOARD_STICK_Y=0 never moves the stick, =1
 * always does. */
static int keyboard_stick_y(void)
{
    static int mode = -2;
    if (mode == -2) {
        const char *v = getenv("RECOMP_KEYBOARD_STICK_Y");
        mode = v && *v == '0' ? 0 : v && *v == '1' ? 1 : -1;
    }
    if (mode >= 0)
        return mode;
    return !(xbox_keyboard_driving && xbox_keyboard_driving());
}

static SHORT axis(int negative, int positive)
{
    int v = 0;
    if (s_down[negative]) v -= 32767;
    if (s_down[positive]) v += 32767;
    return (SHORT)v;
}

/* One axis from two key pairs (numpad or letters), clamped to the stick. */
static SHORT axis2(int neg1, int pos1, int neg2, int pos2)
{
    int v = axis(neg1, pos1) + axis(neg2, pos2);
    return (SHORT)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

/* Merge the keyboard into a state (a real pad's, or an empty one): buttons
 * OR'ed, analog buttons at full pressure, sticks where a key is held. */
void xbox_sdl_keyboard_state(XBOX_INPUT_STATE *s)
{
    WORD b = 0;
    BYTE *an = s->Gamepad.bAnalogButtons;
    SHORT x;

    if (s_down[SDL_SCANCODE_I])         b |= XBOX_GAMEPAD_DPAD_UP;
    if (s_down[SDL_SCANCODE_K])         b |= XBOX_GAMEPAD_DPAD_DOWN;
    if (s_down[SDL_SCANCODE_J])         b |= XBOX_GAMEPAD_DPAD_LEFT;
    if (s_down[SDL_SCANCODE_L])         b |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (s_down[SDL_SCANCODE_RETURN] || s_down[SDL_SCANCODE_KP_ENTER])
                                        b |= XBOX_GAMEPAD_START;
    if (s_down[SDL_SCANCODE_BACKSPACE] || s_down[SDL_SCANCODE_TAB]
        || s_down[SDL_SCANCODE_AC_BACK])  /* Android's Back */
                                        b |= XBOX_GAMEPAD_BACK;
    if (s_down[SDL_SCANCODE_Z])         b |= XBOX_GAMEPAD_LEFT_THUMB;
    if (s_down[SDL_SCANCODE_C])         b |= XBOX_GAMEPAD_RIGHT_THUMB;
    s->Gamepad.wButtons |= b;

    if (s_down[SDL_SCANCODE_A] || s_down[SDL_SCANCODE_SPACE]) an[XBOX_BUTTON_A] = 255;
    if (s_down[SDL_SCANCODE_B]) an[XBOX_BUTTON_B] = 255;
    if (s_down[SDL_SCANCODE_X]) an[XBOX_BUTTON_X] = 255;
    if (s_down[SDL_SCANCODE_Y]) an[XBOX_BUTTON_Y] = 255;
    if (s_down[SDL_SCANCODE_Q] || s_down[SDL_SCANCODE_LSHIFT] || s_down[SDL_SCANCODE_RSHIFT])
        an[XBOX_BUTTON_WHITE] = 255;
    if (s_down[SDL_SCANCODE_E] || s_down[SDL_SCANCODE_LCTRL] || s_down[SDL_SCANCODE_RCTRL])
        an[XBOX_BUTTON_BLACK] = 255;
    if (s_down[SDL_SCANCODE_DOWN]) an[XBOX_BUTTON_LTRIGGER] = 255;
    if (s_down[SDL_SCANCODE_UP])   an[XBOX_BUTTON_RTRIGGER] = 255;

    if ((x = axis(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT)) != 0) s->Gamepad.sThumbLX = x;
    if (keyboard_stick_y() && (x = axis(SDL_SCANCODE_DOWN, SDL_SCANCODE_UP)) != 0)
        s->Gamepad.sThumbLY = x;
    if ((x = axis2(SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6, SDL_SCANCODE_F, SDL_SCANCODE_H)) != 0)
        s->Gamepad.sThumbRX = x;
    if ((x = axis2(SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_8, SDL_SCANCODE_G, SDL_SCANCODE_T)) != 0)
        s->Gamepad.sThumbRY = x;
}

#endif
