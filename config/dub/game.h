/* Midnight Club 3: DUB Edition -- the guest addresses shared code needs
 * (src/main.c). Everything address-bound beyond these lives in
 * src/games/dub/recomp_manual.c. */
#pragma once

#define MC3_ADDR_PB_STATE    0x003ECAF0u   /* D3D's push-buffer state (fence wait, +0x2C) */
#define MC3_ADDR_FRAME_TIME  0x0047C404u   /* the game's frame time, float (sub_001EDBE0) */
#define MC3_CODE_LO          0x00011000u   /* .text .. last code section (XPP): stack walks */
#define MC3_CODE_HI          0x00411000u

/* Settings this game starts with unless mcdub3_env.txt (or the environment)
 * says otherwise (src/main.c, set_runtime_defaults).
 * RECOMP_GL_RT_EDGE=1: the speed blur samples the frame past its image's
 * edge; the strip there is filled with the edge, so no white lines at the
 * picture's edges at speed (as in the Remix, 2026-11-02). */
#define MC3_GAME_DEFAULTS(X) \
    X("RECOMP_GL_RT_EDGE", "1") \
    X("RECOMP_GL_FILL_STEADY", "1")   /* loading screens: the 4:3 fill no longer flickers */
