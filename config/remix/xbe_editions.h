/* Every retail default.xbe of the Remix this lift runs, checked at boot
 * (src/main.c). Same format as config/dub/xbe_editions.h:
 *
 *   X(name, FNV-1a 64 of the whole default.xbe, certificate region, version)
 *
 * The European disc is the lifted one (2026-10-24). The USA disc joined on
 * 2026-10-25: tools/xbe_edition.py found its code the same but for one
 * branch (ja -> jae at 0x001525FC), lifted both ways (edition_variants.json)
 * and chosen by MC3_XBE_VARIANTS below. Another edition joins only after
 * that comparison: a disc whose code differs more needs a lift of its own.
 * The name is used for game-remix-<name>/ folders and EDITION=. */
#pragma once

#define MC3_XBE_EDITIONS(X) \
    X("eur", 0x4438fd3ef9473ce0ull, 0x4u, 0x6u)   /* PAL, En/Fr/De/Es/It (Redump: Europe) */ \
    X("usa", 0xb9644e6610d2f14aull, 0x1u, 0x5u)   /* NTSC-U (Redump: USA) */

/* The editions whose branches differ from the lifted disc's, with their
 * variant number in edition_variants.json (main.c sets
 * g_recomp_xbe_variant; any other edition is 0). */
#define MC3_XBE_VARIANTS(V) \
    V("usa", 1)
