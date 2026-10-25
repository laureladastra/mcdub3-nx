# 9. Editions whose code differs in a branch share the lift

Date: 2026-10-25

## Context

A lift is the machine code of one `default.xbe`. Another edition's disc has
run on it when its code is byte for byte the same (the DUB Edition's
European disc next to its USA one: only the header differs). The Remix's
USA disc, compared with its European (lifted) one, differs in its header,
19 bytes of `.rdata` (a timestamp and a key, both read from the disc at run
time) and **one byte of code**: `ja` becomes `jae` at 0x001525FC
(`cmp byte ptr [0x0053F80C], 0x1E`). A second lift for one byte would
double the generated C, the overrides and the build per game.

## Decision

- **Edition variants**: `config/<game>/edition_variants.json` lists the
  branches whose condition differs, with each edition's mnemonic and the
  lifted disc's as a check. The translator (`--edition-variants`) lifts
  such a `jcc` with every condition, chosen at run time by
  `g_recomp_xbe_variant` (0: the lifted disc's). The title sets it at boot
  from the disc its edition check identified (`MC3_XBE_VARIANTS` in
  `config/<game>/xbe_editions.h`). A listed branch that the lift does not
  meet, or whose instruction differs from `base`, stops the lift.
- Only branch conditions (a `jcc` opcode, same target). Any other code
  difference still needs a lift of its own.
- `make xbe-edition XBE=<disc>/default.xbe` (`tools/xbe_edition.py`) says
  which case a disc is and prints the lines to add.
- CI builds every `APP_EDITIONS` entry of every game; a new edition needs
  no workflow change.

## Consequences

- The Remix ships `mcdub3remix-nx-eur.nro` and `mcdub3remix-nx-usa.nro`
  from one lift; the runner keeps only the European Remix disc.
- One comparison per variant branch at run time (`sub_00152420`, once per
  frame or less): nothing measurable.
- Overrides of a function holding a variant branch must keep both
  behaviours.
