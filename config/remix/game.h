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
 * Merging draws with per-draw matrices off (Switch rain races, 2026-10-28):
 *   merging off                          1262 calls x 21 us, 20 fps
 *   merging + per-draw matrices          ~900 calls x 36 us, 17.5-18.5 fps
 *   merging, per-draw matrices off        975 calls x 20 us -- the cheapest
 * The Remix changes matrices often; each merged call with several matrix
 * sets uploads two 64-row uniform arrays, and Mesa resends the program's
 * whole uniform block for each. The DUB Edition keeps both on.
 * RECOMP_GL_CUBO=1: the vertex programs' 192 constants streamed through a
 * uniform buffer and bound by range -- not copied, 3 KB at a time, into the
 * driver's command stream at each of ~270 changes a rain frame. */
#define MC3_GAME_DEFAULTS(X) \
    X("RECOMP_GL_MERGE", "1") \
    X("RECOMP_GL_PERDRAW", "0") \
    X("RECOMP_GL_MERGE_KB", "256") \
    X("RECOMP_GL_CUBO", "1")          /* constants through a uniform buffer */ \
    X("RECOMP_GL_LEAN", "1")          /* base-vertex draws, changed uniforms only */ \
    X("RECOMP_GL_SHARPEN", "0")       /* no CAS: part of the present pass's ~2.7 ms of GPU */
