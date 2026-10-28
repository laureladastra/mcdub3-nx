/* Midnight Club 3: DUB Edition Remix -- the guest addresses shared code
 * needs (src/main.c). Mapped from the DUB Edition's (config/dub/game.h)
 * with tools/match_lifts.py's Mapper, 2026-10-24: the same operand of the
 * twin instruction in identical functions. */
#pragma once

#define MC3_ADDR_PB_STATE    0x00422D80u   /* D3D's push-buffer state: 12 of 12 references agree */
#define MC3_ADDR_FRAME_TIME  0x004B2D70u   /* the game's frame time: 1 of 1 */
#define MC3_CODE_LO          0x00011000u   /* .text .. XPP (ends 0x004476C8) */
#define MC3_CODE_HI          0x00448000u

/* Settings this game starts with unless mcdub3remix_env.txt (or the
 * environment) says otherwise (src/main.c, set_runtime_defaults).
 * RECOMP_GL_MERGE=0: merging draws gave the Remix's rain races a frame over
 * 80 ms about every second (1.2/s, against 0.2/s without; Switch,
 * 2026-10-28) -- the stutter -- where the DUB Edition gains from it. */
#define MC3_GAME_DEFAULTS(X) \
    X("RECOMP_GL_MERGE", "0")
