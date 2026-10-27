# Contributing: development guide

The technical source of truth: how the project turns the Xbox game into a
Switch program, what sits where, how to set up the development VM, and the
day-to-day loop. Decisions and their reasons: [docs/decisions/](docs/decisions/).

## Quick start

### Mac and Linux

Put the extracted disc (`default.xbe` + data) in `game/`, run the following commands from the repo root:

```sh
make setup        # once: build deps, devkitPro, Python venv
make lift         # parse the XBE, write config/<game>/xbe_facts.h, lift to C
make build run    # Linux or macOS build + run in a window
make switch       # Switch NRO + SD staging in work-<game>/switch_sd (HD=1: with the texture pack)
make nsp          # NSP forwarder for the home menu
make mac-app      # Mac app (SCALE=5, UI_SCALE=5 by default, SETTINGS="K=V ...", HD=1): work-<game>/mac_app/
make hd-dump      # texture packs: dump, then make hd-tools hd-upscale
make              # every target, grouped
```

### Nintendo Switch

Build the software and copy the files to the console with the following commands:

```sh
make switch       # Switch NRO + SD staging in work-<game>/switch_sd (HD=1: with the texture pack)
make nsp          # NSP forwarder for the home menu
```

The files will be organized according to the following hiërarchy:

```
sdmc:/switch/mcdub3-nx/
├── mcdub3-nx-usa.nro  the NRO for your edition of the game (mcdub3-nx-<edition>.nro)
├── mcdub3_env.txt   settings: KEY=VALUE per line, every switch explained inside
│                    (template: switch/mcdub3_env.example.txt)
├── mcdub3_log.txt   written when not launched through nxlink
├── mcdub3_log_prev.txt  the run before (kept one start, for hangs reported late)
├── game/            the extracted disc (not the ISO)
└── hd/textures/     optional texture pack (make switch HD=1)
```

Start with title takeover (hold **R** while launching a game) if the NRO forwarder is not installed to ensure 4GB of RAM is allocated to the game.

## How it works: two stages

```
STAGE 1: LIFT (Python)                            STAGE 2: BUILD (C)
game/default.xbe ─► xboxrecomp/tools ─► work-<game>/gen/*.c ─► CMake + GCC ─► work-<game>/build-linux/mcdub3
 (x86 machine code)  parse, disassemble,  (the game as C)  + src/*.c          or devkitA64 ─► .nro (Switch)
                     identify, translate                   + toolkit runtime
```

**Stage 1, the lift (`make lift`, `make regen`, `make relift`).** The
toolkit's recompiler is a set of Python programs (`xboxrecomp/tools/`). They
read the Xbox executable, disassemble it (with the *capstone* library), find
every function, recognise library code, recover how each function takes its
arguments, and translate every x86 function into a C function. Purpose: turn
machine code that only runs on an Xbox CPU into portable C that any compiler
can build. Output: `work-<game>/gen/` (~90 files, ~3 M lines) plus intermediate
results in `work-<game>/analysis/`. It runs once, then again only when the C must be
regenerated: after changing seeds (`config/<game>/seed_functions.json`, full
`make regen`), or hand-written overrides in `src/games/<game>/recomp_manual.c` / the
translator (`make relift`, skips the analysis steps).

The stage-1 steps, each a Python tool in `xboxrecomp/tools/`:

| Step        | Tool (`xboxrecomp/tools/`)                              | Does                                                        | Output                   |
| ----------- | ------------------------------------------------------- | ----------------------------------------------------------- | ------------------------ |
| parse       | `xbe_parser`                                            | reads the XBE header: sections, entry point, kernel imports | `work-<game>/analysis/xbe_*`    |
| disassemble | `disasm` (uses **capstone**, a Python x86 disassembler) | finds every function and jump target                        | `work-<game>/analysis/disasm/`  |
| identify    | `func_id`                                               | recognises library code (C runtime, XDK)                    | `work-<game>/analysis/func_id/` |
| ABI         | `abi_analysis`                                          | recovers calling conventions and argument counts            | `work-<game>/analysis/abi/`     |
| translate   | `recomp`                                                | turns each x86 function into a C function                   | `work-<game>/gen/`              |
| (helpers)   | `seed_from_log`, `xiso`                                 | seed candidates from a run log; ISO unpacking               | `config/`, `game/`       |

**Stage 2, the build (`make build`, `make switch`).** Plain C: CMake + GCC
(Linux) or devkitA64 (Switch) compile three things into one program:

- `work-<game>/gen/`: the game itself, as generated C;
- `xboxrecomp/src/`: the toolkit's runtime, which stands in for the Xbox: its
  kernel, memory map, GPU, sound chip and controllers;
- `src/`: our own code: boot (`main.c`), hand-written function replacements
  (`recomp_manual.c`), Switch glue (`switch_nx.c`).

Purpose: produce a native program for the host: a Linux binary to debug in a
window, or a Switch `.nro`. No Python runs here; after editing C, `make build`
is all it takes.

**Why two stages:** lifting is slow and only depends on the XBE, seeds and
overrides; building is the part you repeat all day. Keeping them apart means
an ordinary code change never re-runs the lift.

## How our changes sit on top of the game

**The generated C in `work-<game>/gen/` is never edited.** It is a build product,
like an object file: rewritten from scratch by every lift, and never committed.
The game itself (`game/default.xbe`) is never edited either. Everything we
change lives in our own files, applied on top of the untouched base every
time:

```
   game/default.xbe  (the base: never changes)
          │
          │  stage 1 lift, steered by our inputs:
          │    config/<game>/seed_functions.json   extra function starts
          │    src/games/<game>/recomp_manual.c          which functions NOT to generate
          │    xboxrecomp/tools/            translator fixes
          ▼
   work-<game>/gen/*.c  (disposable output)
          │
          │  stage 2 build, linked with our code:
          │    src/games/<game>/recomp_manual.c          our versions of those functions
          │    src/main.c, src/switch_nx.c  boot, data patches, platform glue
          │    xboxrecomp/src/              the runtime: the "Xbox hardware"
          ▼
   mcdub3 / .nro
```

How each kind of change is applied:

| To change                                           | Put it in                                                                                                    | How it takes effect                                                                                                                                         |
| --------------------------------------------------- | ------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| a game function (bug fix, native replacement, stub) | `src/games/<game>/recomp_manual.c`: define `void sub_XXXXXXXX(void)`                                                      | the lift skips that function (`--exclude-manual`); the linker uses ours, for direct calls and indirect calls (the dispatch table points at the same symbol) |
| a game function, but keep the original              | declare `extern void sub_XXXXXXXX_gen(void);`, define `sub_XXXXXXXX` and call `sub_XXXXXXXX_gen()` inside it | the lift emits the original body under the `_gen` name, so ours wraps it (trace, fix up arguments or results)                                               |
| keep an override around but switched off            | wrap it in `#if 0 … #endif`                                                                                  | the lift ignores it and generates the original again (relift)                                                                                               |
| a missing function                                  | `config/<game>/seed_functions.json` (with a note)                                                                   | the disassembler treats it as a function start; it gets lifted like the rest                                                                                |
| game data or settings (a constant, a limit)         | `src/main.c`, after the memory is mapped: write to the guest address                                         | patched in memory at boot; the XBE file stays untouched                                                                                                     |
| how the Xbox hardware or kernel behaves             | `xboxrecomp/src/` (commit as `xboxrecomp:`)                                                                  | rebuilt and linked                                                                                                                                          |
| how x86 is translated                               | `xboxrecomp/tools/recomp/` (commit as `xboxrecomp:`)                                                         | the next lift produces different C                                                                                                                          |

So "moving our changes back into the source" never happens: our changes *are*
source (`src/`, `config/`, `xboxrecomp/`), and each lift re-creates the base
from the XBE. A new lift never loses a fix, because no fix lives in `work-<game>/gen/`.

### Regen or just build?

| You changed                                                                        | Run                 | Why                                                                             |
| ---------------------------------------------------------------------------------- | ------------------- | ------------------------------------------------------------------------------- |
| `src/main.c`, `src/switch_nx.c`, `xboxrecomp/src/` (runtime)                       | `make build`        | plain C; the lift is unaffected                                                 |
| the *body* of an existing override in `src/games/<game>/recomp_manual.c`                        | `make build`        | same set of functions replaced; only our code changed                           |
| *added or removed* an override in `src/games/<game>/recomp_manual.c`                            | `make relift build` | the lift must drop (or restore) the lifted body; fast: skips the analysis steps |
| `xboxrecomp/tools/recomp/` (translator)                                            | `make relift build` | same analysis, different C                                                      |
| `config/<game>/seed_functions.json`, `xboxrecomp/tools/disasm`, `func_id`, `abi_analysis` | `make regen build`  | the analysis itself changes (which functions exist)                             |
| the disc (`game/`)                                                                 | `make lift build`   | new base                                                                        |

`make build` checks this for you: it stops, and names the step to run, when
the seeds are newer than the lifted C or the set of overrides in
`src/games/<game>/recomp_manual.c` differs from the one the last lift saw (read with the
toolkit's own scanner, `xboxrecomp/tools/recomp/manual_scan.py`, so the check
and the lifter always agree). Any of `relift`
and `regen` rewrites `work-<game>/gen/`, so the next build recompiles every chunk
(the long build); a plain `make build` only recompiles what changed.

### Files and paths on Linux and the Switch

The game only ever uses Xbox paths. One toolkit layer translates them to two
host folders, and only those two folders differ per platform:

```
game (lifted code)        \Device\CdRom0\..., D:\..., \Device\Harddisk0\Partition1\UDATA\...
        │
        ▼  xboxrecomp/src/kernel/kernel_path.c  (same code on Linux and Switch)
        │
        ├── game_dir   disc files; Partition1 (TDATA/UDATA: settings, saves)
        └── save_dir   T:\ U:\ Z:\ drive letters, partition images
```

|            | Linux (debug)                                            | Switch                                                   |
| ---------- | -------------------------------------------------------- | -------------------------------------------------------- |
| `game_dir` | `<repo>/game`, or `<repo>/game-<edition>` with `EDITION=` (tools/env.sh → `MC3_GAME_DIR`) | `sdmc:/switch/mcdub3-nx/game`                            |
| `save_dir` | `<repo>/work-<game>/save` (tools/run_linux.sh → `MC3_SAVE_DIR`) | `sdmc:/switch/mcdub3-nx/save`                            |
| chosen by  | the scripts, through environment variables               | `src/main.c` defaults (`#ifdef __SWITCH__`)              |
| override   | `MC3_GAME_DIR=…`, `MC3_SAVE_DIR=…`                       | the same keys in `sdmc:/switch/mcdub3-nx/mcdub3_env.txt` |

Editions: the USA and European discs carry the same program (config/<game>/xbe_editions.h lists
every accepted default.xbe; the boot check names the one found, `disc: edition eur`). One lift
and one build run them all; the discs sit side by side in `game/` and `game-<edition>/`
(gitignored), `EDITION=eur` picks one for `make extract`, `make run` and `make build` on Windows.
A new edition needs a comparison of its XBE first: different code needs a lift of its own.

So nothing about files is decided twice: the scripts in `tools/` only exist on
the development machine and set the Linux folders; `switch/build.sh` stages the
same layout on the SD card (`work-<game>/switch_sd/switch/mcdub3-nx/`); the game code and
the path layer are identical on both.

Profiles (`game_dir/UDATA/54540079/<12 hex>/`) are signed with the port's
all-zero key, so they move freely between Linux, Mac and Switch. Profiles
from a real Xbox are signed with its key and need `make import-save FILE=…`
(tools/save_import.py: reads memory unit dumps, `.gsx`, folders, `.mc3`; PS2 `.max`/`.sps` read and refused, another format;
re-signs; backs up to `work-<game>/saves-backup/`). Details in CLAUDE.md, Saves.

## Two games: GAME=dub and GAME=remix

The repo builds two games (docs/decisions/0006): *Midnight Club 3: DUB
Edition* (`GAME=dub`, the default) and *DUB Edition Remix* (`GAME=remix`).
Add `GAME=remix` to any target: `make lift build run GAME=remix`,
`make switch GAME=remix EDITION=eur`.

| | DUB Edition | Remix |
|---|---|---|
| config | `config/dub/` | `config/remix/` |
| overrides | `src/games/dub/recomp_manual.c` | `src/games/remix/recomp_manual.c` |
| discs | `game/`, `game-<edition>/` | `game-remix/`, `game-remix-<edition>/` |
| generated | `work-dub/` | `work-remix/` |
| executable, Switch folder | `mcdub3`, `mcdub3-nx/` | `mcdub3remix`, `mcdub3remix-nx/` |
| NRO | `mcdub3-nx-<edition>.nro` | `mcdub3remix-nx-<edition>.nro` |
| app identity (Switch, Mac, Windows) | `config/dub/app.env` | `config/remix/app.env` |

Editions: each game runs all its editions on one lift. The DUB Edition's
USA and European discs share their code exactly; the Remix's USA disc
differs from its European (lifted) one in a single branch condition, which
the lift carries both ways (an *edition variant*, below). To add a disc:

```sh
make xbe-edition GAME=remix XBE=game-remix-usa/default.xbe
```

compares its `default.xbe` with the lifted one and prints what to add:

- **the same code** (only the header or data differ, as between editions
  usually): one `X(...)` line in `config/<game>/xbe_editions.h`, the
  edition in `APP_EDITIONS` and a random `APP_TITLE_ID_<edition>` in
  `config/<game>/app.env`;
- **the same code but for branch conditions** (a `jcc` whose opcode alone
  changed): the same, plus the branches in
  `config/<game>/edition_variants.json` and the edition's variant number in
  `MC3_XBE_VARIANTS` (`xbe_editions.h`); then `make relift`. The lifter
  emits each such branch with both conditions, and the runtime picks one by
  `g_recomp_xbe_variant`, which `src/main.c` sets from the disc it
  identified (`disc: edition usa, branch variant 1` in the log);
- **other code differences**: a lift of its own, which the layout does not
  support yet.

The CI build then makes the new edition's NRO by itself (it builds every
`APP_EDITIONS` entry of every game).

A fix for one game goes in its own `config/<game>/` or `src/games/<game>/`;
the other game never builds it. A fix that holds for both goes in shared
code (everything else). Shared code takes no game's addresses: what it needs
comes from `config/<game>/game.h`.

The Remix's overrides started as a port of the DUB Edition's, every address
rewritten (2026-10-24):

```
tools/match_lifts.py  game-usa/default.xbe work-dub/analysis/disasm/functions.json \
    game-remix-eur/default.xbe work-remix/analysis/disasm/functions.json \
    --manual src/games/dub/recomp_manual.c --seeds config/dub/seed_functions.json
tools/port_overrides.py <the same four> --src src/games/dub/recomp_manual.c \
    --out src/games/remix/recomp_manual.c --map config/remix/port_map.txt
```

`match_lifts.py` pairs the two lifts' functions (identical code with addresses
masked, neighbours in order, similar code, shifted code) and reports where
each override and seed of one build sits in the other. `port_overrides.py`
rewrites a file through it, data addresses too (the same operand of the twin
instruction); what it cannot map stays and is marked `PORT?`. Use them to
carry a new DUB Edition fix over, or to find a function in the other build.

## Project layout

What sits where, who writes it, and whether git tracks it. Every path is
relative to the repo root; the scripts find the root from their own location.

Legend: **[git]** tracked · **[ignored]** gitignored (local only) ·
**[generated]** written by a tool, safe to delete and regenerate.

```
mcdub3-nx/
├── Makefile                  [git] the development loop; `make` lists targets
├── CMakeLists.txt            [git] stage 2: one C project, Linux and Switch hosts
├── README.md                 [git] what this is, quick start
├── CONTRIBUTING.md           [git] technical source of truth: stages, layout, VM, the loop
├── CLAUDE.md                 [git] engineering notes: rules, facts about MC3, status
├── TODO.md                   [ignored] personal work list: bugs, features, decisions to evaluate
├── .gitignore                [git]
│
├── src/                      [git] OUR C code, compiled with the lifted code
│   ├── main.c                  boot: load XBE, map Xbox memory, start devices, jump to the game
│   ├── games/<game>/           per game (dub, remix): only the selected game's is built
│   │   └── recomp_manual.c       hand-written replacements for specific lifted functions
│   ├── ac97_nabm.c             AC'97 audio controller registers (reset bits complete at once)
│   ├── difftest.c              guest snapshot around one call, for tools/difftest.py
│   ├── bink_native.c           movies: FFmpeg decodes Bink video and plays the soundtrack
│   └── switch_nx.c             Switch only: log file, settings file, crash handler
│
├── config/<game>/            [git] per-game inputs to stage 1 and 2 (dub, remix)
│   ├── game.env                names and folders: app name, Switch folder, disc folders
│   ├── game.h                  the guest addresses shared code needs (main.c)
│   ├── seed_functions.json     extra function start addresses for the disassembler (see "seeds")
│   ├── xbe_editions.h          every disc (edition) the lift runs, checked at boot
│   ├── xbe_facts.h             entry point + title ID, written by `make lift`, used by main.c
│   └── port_map.txt            remix only: hand-found DUB -> Remix addresses (tools/port_overrides.py)
│
├── tools/                    [git] scripts behind the make targets
│   ├── env.sh                  shared paths (game/, work-<game>/), picks a Python with capstone
│   ├── first_lift.sh           make lift: parse + write xbe_facts.h + full regen
│   ├── regen.sh                make regen / relift: the stage-1 pipeline
│   ├── build_linux.sh          make build: CMake + Ninja into work-<game>/build-linux*
│   ├── run_linux.sh            make run / run-gdb / run-headless / snapshot / trap
│   ├── snapshot_report.py      make report: a crash/watchdog snapshot, explained
│   ├── difftest.py             make difftest: replay a captured call on x86, diff vs lifted
│   ├── profile.sh              make profile: perf per thread, per library, per function
│   ├── record.sh               make record: frames + audio of a run (WINDOW=1, VIDEO=0)
│   ├── audio_diff.py           a movie's soundtrack vs the captured output: holes, skips, drift
│   └── extract_disc.sh         make extract: ISO → game/
├── switch/
│   ├── build.sh              [git] the NRO build + SD-card staging (runs in the container)
│   ├── nsp.sh                [git] make nsp: the home-menu forwarder (NTON's recipe + hacBrewPack)
│   ├── docker_build.sh       [git] make switch / nsp: a script inside devkitPro's pinned image
│   ├── devkitpro-image       [git] the pinned image (tag@digest), shared with CI
│   └── app.env               [git] what the games' packages share: author, pinned NSP tools
│                                 (each game's identity: config/<game>/app.env)
├── vm/
│   └── setup-ubuntu.sh       [git] make setup: packages, Docker + devkitPro image, Python venv
│
├── docs/
│   ├── decisions/            [git] ADRs: why things are the way they are
│   │   ├── 0001-native-aarch64-not-carafe.md
│   │   ├── 0002-vendored-toolkit-own-snapshot.md
│   │   └── 0003-linux-vm-debug-loop.md
│   ├── upstream.md           [git] fixes and issues to send upstream (xboxrecomp / nfsu2-sw)
│   └── drafts/               [ignored] unpublished texts (e.g. the nfsu2-sw licensing issue)
│
├── xboxrecomp/               [git] THE TOOLKIT, vendored (ADR 0002): our own copy, edited in place
│   ├── tools/                  stage 1, Python: xbe_parser, disasm, func_id, abi_analysis,
│   │                           recomp, seed_from_log, xiso, ... (+ their unit tests)
│   ├── src/                    stage 2, C runtime libraries linked into the game:
│   │   ├── kernel/               Xbox kernel replacement: memory map, files, threads, sync
│   │   ├── platform/             host layer: Win32 API on Linux/Switch (POSIX, libnx)
│   │   ├── nv2a/                 GPU register model (from xemu)
│   │   ├── nv2a_gl/  nv2a_vk/    renderers: OpenGL (Linux) / Vulkan (Switch NVK, Linux)
│   │   ├── apu/  audio/          sound chip model (from xemu), DirectSound, SDL output
│   │   ├── usb/  input/          USB controllers the game talks to; host pads, Joy-Cons
│   │   ├── d3d/  video/          D3D8 layer, movie/framebuffer helpers
│   ├── templates/runtime/      recomp_types.h: the register model the generated C includes
│   ├── include/                public header (xboxrecomp.h)
│   └── docs/  tests/           toolkit documentation and C tests
│
├── game/                     [ignored] the extracted disc (game data; never committed)
│   ├── default.xbe             the game executable: input to stage 1, loaded again at runtime
│   ├── default_analysis.json   [generated] XBE parse, read by the disassembler
│   ├── TDATA/, UDATA/          [generated] the title's settings and saves (Xbox hard disk partition 1)
│   ├── assets_x.dat(_1.._5), banks.dat, audio/, flash/, media/, resources/, video/ (Bink .bik)
│   └── dashupdate.xbe, downloader.xbe, update.xbe   Xbox system tools on the disc; not used
│
└── work-<game>/                     [ignored] EVERYTHING GENERATED; delete any part and rebuild it
    ├── analysis/             [generated] stage-1 intermediate results (make lift / regen)
    │   ├── xbe_summary.txt, xbe_analysis.json    the XBE parse
    │   ├── disasm/             functions.json, labels.json, xrefs.json, strings.json, asm/
    │   ├── func_id/            library functions recognised
    │   ├── abi/                calling conventions
    │   ├── recomp/summary.json translation statistics
    │   └── recomp.log          full translator output (incl. untranslated-instruction tally)
    ├── gen/                  [generated] stage-1 OUTPUT: the game as C (~90 files, ~3 M lines)
    │   ├── recomp_0000.c … recomp_0086.c   the lifted functions, 250 per file
    │   ├── recomp_dispatch.c               address → function table (indirect calls)
    │   ├── recomp_stubs_unresolved.c       placeholders for calls to undetected functions
    │   ├── recomp_funcs.h                  declarations of every lifted function
    │   └── recomp_types.h                  copy of the toolkit's register model
    ├── build-linux/          [generated] stage-2 build tree (make build): CMake/Ninja files,
    │   │                       object files for gen/, src/ and the toolkit runtime
    │   └── mcdub3          the Linux executable
    ├── build-linux-vk/, build-linux-dbg/      variants (VULKAN=1, DEBUG=1)
    ├── build-switch/, build-switch-vk/        Switch build trees (make switch); mcdub3.elf -> mcdub3.nro
    ├── switch_sd/            [generated] SD-card staging: switch/mcdub3-nx/{mcdub3-nx-<edition>.nro, game/}
    ├── elf/                  [generated] one ELF per staged NRO, to resolve console crash addresses
    ├── save/                 [generated] the title's drive letters (T:, U:, Z:) and partition images
    ├── logs/                 [generated] every make lift/build/run/switch log, timestamped
    ├── capture/              [generated] make record: frames (BMP), audio.wav, capture.mp4
    ├── venv/                 [generated] Python + capstone for stage 1 (make setup)
    └── deps/                 [optional] Switch extras built by hand: mesa NVK SDK, glslang, FFmpeg
```

### What to delete when

| Want to              | Delete                                         | Then         |
| -------------------- | ---------------------------------------------- | ------------ |
| Rebuild from scratch | `make clean-build` (`work-<game>/build-*`)            | `make build` |
| Re-lift from scratch | `make clean-gen` (`work-<game>/gen`, `work-<game>/analysis`) | `make lift`  |
| Free disk            | old `work-<game>/build-*`, `work-<game>/logs/*`              | nothing      |
| Reset Python         | `work-<game>/venv`                                    | `make setup` |

Never needed: deleting anything outside `work-<game>/` (and `game/` only holds the disc).

## Development environment

One Ubuntu ARM64 VM on the Mac (Apple Silicon) does everything: lifting, the
windowed Linux build with gdb, and Switch cross-builds. Parallels Desktop or
UTM both work. Why: [ADR 0003](docs/decisions/0003-linux-vm-debug-loop.md).

## Native macOS build (Apple Silicon)

The same lifted C builds and runs natively on an Apple Silicon Mac
(2026-10-21), next to the VM: the trees are `work-<game>/build-mac*`, so the Mac
and the VM can share `work-<game>/`. The lift itself still runs in the VM (or
anywhere with the Python pipeline); `make build run` on the Mac needs only
the lifted C in `work-<game>/gen/`.

```sh
/opt/homebrew/bin/brew install cmake ninja pkgconf sdl2 ffmpeg libepoxy
make build run
```

- Use the Apple Silicon Homebrew (`/opt/homebrew`). The scripts re-run
  themselves as arm64 when started from a Rosetta shell
  (`mc3_mac_native` in tools/env.sh), so an Intel Homebrew in `/usr/local`
  is never picked up.
- Rendering is OpenGL 4.1 on Metal (Apple's GL); the renderer asks for a
  forward-compatible 3.3 core context there.
- macOS lets only the main thread touch windows and events, so the game runs
  on a thread of its own and the main thread serves window creation and the
  event pump (xboxrecomp/src/platform/host_main.c).
- No core pinning (macOS has no thread affinity); `HEADLESS=1` (xvfb) is
  Linux only.

### The Mac app

`make mac-app` packages the build as `work-<game>/mac_app/Midnight Club 3.app`
(tools/mac_app.sh): self-contained (Homebrew's libraries copied in with
dylibbundler, ad-hoc signed), so it can go to /Applications. It keeps its
data like the Switch's SD folder, in `~/Library/Application Support/mcdub3/`:
`game/` (the disc; on first launch the app opens the folder and asks for
it), `hd/textures/`, `save/`, `mcdub3_env.txt` (settings, written from the
documented template on first launch), `mcdub3_log.txt`.

```sh
make mac-app                                             # defaults: SCALE=5 (3600x2400), UI_SCALE=5
make mac-app SCALE=1.5 UI_SCALE=2                        # 1280x720, for a slower Mac
make mac-app SCALE=2 SETTINGS="RECOMP_GL_SHARPEN=0.3 MC3_MOVIE_VOLUME=80"
make mac-app HD=1                                        # texture pack inside the app
make mac-app DISC=$PWD/game                              # link this disc on first launch
```

Settings apply in order, later wins: the defaults built in (SCALE,
SETTINGS, HD), then `mcdub3_env.txt` in Application Support. Every
`RECOMP_*` / `MC3_*` setting works there (README, Settings).

## Windows build (in testing since 2026-10-23)

Windows builds the same lifted C with MSYS2's clang: x86-64 in the CLANG64
environment, Windows on ARM in CLANGARM64. Trees are
`work-<game>/build-windows-<arch>/`; `dist/` in there holds `mcdub3.exe`, the DLLs
it needs (tools/windows/copy_dlls.py reads the import tables), the
settings (`mcdub3_defaults.txt` from SCALE / UI_SCALE / SETTINGS, rewritten
by each build; `mcdub3_env.txt`, the player's, kept) and `run_mcdub3.bat`.
The exe needs no launcher (src/windows_host.c): started on its own it reads
both settings files, finds `game\` beside it or the repo's, writes its log
to `work\logs\windows_run_<time>.log` and closes its console; a native
build makes or refreshes the desktop shortcut to it.

- **Players:** `install_windows.bat` in the repo root, double-clicked
  (README, Installation). It unpacks MSYS2 into `C:\msys64` if missing
  (`MSYS2_ROOT` for another folder), updates it, and runs
  tools/install_windows.sh in the shell matching the machine: `make setup`
  (tools/setup_windows.sh, pacman: clang, cmake, ninja, SDL2, FFmpeg,
  Python with capstone and Pillow), an ISO in `game\` unpacked, `make lift`
  if needed, `make build`, a desktop shortcut. Re-runnable.
- **Developers on Windows:** the CLANG64 / CLANGARM64 shell (`tools\windows\shell.bat`
  opens the right one in the repo; the installer's MSYS2 has no Start menu
  entries), then the usual `make setup`, `make lift`, `make build run`.
- **Cross build from macOS or Linux:** `make windows` (`WIN_ARCH=aarch64`
  for ARM), tools/build_windows.sh with llvm-mingw (a release unpacked in
  `work-<game>/tools/`) and tools/windows/toolchain.cmake; tools/win_deps.py
  fetches MSYS2's packages and their dependencies into
  `work-<game>/win-sysroot/<arch>/` (Python 3.14+ for zstd). Its tree is
  `work-<game>/build-wincross-<arch>/`, apart from the native one: a `work-<game>/`
  shared with a Windows VM would otherwise hold one CMake tree for two
  paths. Fast for build errors; run the result on Windows (Parallels: the
  repo is `\\Mac\Home\...`).

How it runs: the toolkit's Windows path was Direct3D 11, XAudio2, XInput
and page-fault MMIO (VEH). With `XBOXRECOMP_WIN_SDL_GL` (MC3 turns it on)
Windows takes the desktop path instead: the pushbuffer executor with
nv2a_gl, SDL2 for window, sound and pads, the MMIO registry for the lifted
MMIO sections (`XBOXRECOMP_SDL_HOST` in the sources). The kernel layer is
the toolkit's Win32 code on the real Win32 API (elsewhere it runs on
win32_compat.c); threads are not pinned (platform/win32_host.c).

## Create the VM (Parallels Desktop)

1. Parallels' installation assistant → **Download Ubuntu Linux**. It installs
   Parallels' own ARM64 image (22.04 at the time of writing) with Parallels
   Tools already in it. That is the supported route: on 2026-10-14, Ubuntu's
   24.04 ARM64 desktop ISO failed to boot in Parallels (UEFI file system
   error), and a server install + desktop could not take Parallels Tools.
   22.04 is fine for this project; see "Ubuntu 22.04" below. Before the first
   boot, tick **Customize settings before installation** (or set these
   afterwards with the VM shut down):
   - **Hardware → CPU & Memory:** manual, **6 cores, 10 GB**. Compiling the
     lifted C takes ~1.5 GB per parallel job; the scripts size `JOBS` from
     free memory.
   - **Hardware → Hard Disk:** **64–80 GB**, expanding (the default). An
     expanding disk only uses what is actually written; see the budget below.
   - **Hardware → Graphics:** leave Parallels' default; check it after
     install (below).
   - **Options → Sharing:** share **only the repo folder** (`mcdub3-nx`), not
     the whole home folder. It shows up under `/media/psf/`.
2. Install **Parallels Tools** when prompted: shared folders and the
   graphics driver need it.
3. Symlink the share somewhere short: `ln -s /media/psf/mcdub3-nx ~/mcdub3-nx`.
   Every path is relative to the repo root: the disc in `game/`, everything
   generated in `work-<game>/` (both gitignored). Run the scripts from the root.
4. `cd ~/mcdub3-nx && bash vm/setup-ubuntu.sh`, then open a new shell.

### Ubuntu 22.04

Works for everything except the **Linux Vulkan build** (`VULKAN=1
tools/build_linux.sh`): the toolkit's Vulkan renderer needs glslang 12+ (the
`glslang-default-resource-limits` CMake target), and 22.04 ships 11.8. The
default OpenGL build, the lift pipeline (Python 3.10 is the minimum) and the
Switch builds (devkitPro brings its own toolchain) are unaffected. If you
need Linux Vulkan later, build glslang from source into `work-<game>/deps` and
pass `-Dglslang_DIR`, or move to 24.04.

### Check the VM (5 minutes, before anything else)

```sh
uname -m               # aarch64
getconf PAGESIZE       # 4096 (the Xbox memory mirrors assume 4 KB pages)
nproc; free -g         # the cores/RAM you gave it
df -h ~                # free space on the VM disk
glxinfo -B | grep -E 'renderer|core profile version'
vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|apiVersion'
```

- `OpenGL core profile version` must be **3.3 or higher**.
- `OpenGL renderer`: a Parallels/virtio name means GPU-accelerated. `llvmpipe`
  means software rendering. That still works and is correct, just slower; fine
  for debugging, and performance is only ever measured on the Switch.
- Vulkan: whatever it lists (often only `llvmpipe` = lavapipe on the CPU) is
  fine; the Switch's Vulkan (NVK) gets tested on the console.

If GL is below 3.3: Parallels → Configure → Hardware → Graphics, try the
other options; otherwise run with `LIBGL_ALWAYS_SOFTWARE=1` (llvmpipe, GL 4.5).

### Disk budget

`game/` and `work-<game>/` live in the repo, i.e. on the **Mac's** disk (through the
shared folder); the VM disk only holds Ubuntu and the tools.

| What                                                   | Size                                  |
| ------------------------------------------------------ | ------------------------------------- |
| VM: Ubuntu desktop + build deps + devkitPro            | ~20 GB                                |
| Mac: extracted disc (`game/`)                          | 3.4 GB                                |
| Mac: lifted C + analysis (`work-<game>/gen`, `work-<game>/analysis`) | ~0.5 GB                               |
| Mac: one Linux build tree (`work-<game>/build-linux`)         | ~2–4 GB (lifted C without debug info) |
| Mac: one Switch build tree                             | ~2–4 GB                               |
| Mac: SD staging (`work-<game>/switch_sd`)                     | ~3.4 GB once you build for the Switch |

Disk savers, already the default: the lifted C is built without debug info
(backtraces and breakpoints still name every `sub_XXXXXXXX`;
`GEN_DEBUGINFO=1 tools/build_linux.sh` turns it on when you need line-level
stepping, at a few GB). Delete build trees you're not using
(`rm -rf work-<game>/build-*-lto`); `du -sh work-<game>/*` shows where space went.

**If builds through the shared folder are slow:** `export MC3_WORK=<a folder
on the VM disk>` moves only the generated files (lifted C, builds) off the
share. The disc stays in `game/`.

## Create the VM (UTM, alternative)

Virtualize → Linux → Ubuntu ARM64 ISO, **"Use Apple Virtualization" unchecked**
(the QEMU backend has GPU-accelerated GL), display **virtio-gpu-gl-pci**, the
same CPU/RAM/disk/sharing as above. Then the same checks.

## The loop

Every step is a `make` target in the repo root (`make` lists them all; logs go
to `work-<game>/logs/`). The scripts in `tools/` behind them also work on their own.

```
make lift          # once: parse the XBE, config/<game>/xbe_facts.h, full lift (disc in game/)
make build run     # after a change to src/ or the runtime (window; SECS=n to stop)
make snapshot      # a hang: headless run, guest call stack dumped after 30 s
make report        # explain the newest snapshot: call chain by section, kernel calls
make hoststacks    # every thread's host backtrace after 45 s: who waits on what
make profile       # where the time goes: hottest functions per thread (Linux perf)
make record        # frames + audio of a headless run -> work-<game>/capture/<time>/capture.mp4
make difftest CALL=n  # replay call n of a wrapped function on real x86 (Unicorn), diff vs lifted
make run-gdb       # under gdb; also run-headless, trap
make relift build  # after adding/removing an override, or a translator change
make regen build   # after changing config/<game>/seed_functions.json
make switch send   # when it's time for Eden / the console
```

| Rung                   | When                        | What it catches                                                     |
| ---------------------- | --------------------------- | ------------------------------------------------------------------- |
| Linux build (VM)       | every change                | boot, kernel calls, rendering, audio, game logic                    |
| Eden (Switch emulator) | before the console          | packaging, paths, libnx glue                                        |
| Switch hardware        | regularly, not every change | unaligned atomics, memory/aliasing, threads on Horizon, performance |

The VM's CPU is ARM64 like the Switch's, so most "x86 assumptions" (unaligned
atomics, memory ordering) show up here before the console. The VM's GPU is
virtual: fine for correctness, meaningless for performance.

## Getting the game into the VM

Put the **extracted** disc (`default.xbe` + data) in the repo's `game/`
folder on the Mac (gitignored). The scripts use it from there through the
shared folder; nothing is copied to the VM disk. Run everything from the repo
root:

```sh
cd ~/mcdub3-nx
bash tools/first_lift.sh
```

Only for an ISO: `tools/extract_disc.sh <file.iso>` unpacks it into `game/`.
`MC3_GAME_DIR=<folder>` points the scripts at a disc elsewhere.

## The Switch toolchain: devkitPro's container

`make switch` builds inside devkitPro's official image `devkitpro/devkita64`,
pinned by digest in `switch/devkitpro-image` (ADR 0004). The image exists for
linux/arm64 and linux/amd64, so the same pin serves this VM and GitHub
Actions; it carries devkitA64, libnx, the Switch portlibs (SDL2, Mesa, glm,
FFmpeg) and the build tools (cmake, ninja, python3, rsync). Nothing of the
toolchain is installed on the host: `make setup` installs Docker (Ubuntu's
`docker.io`) and pulls the image. `switch/docker_build.sh` mounts the repo at
the same path and runs `switch/build.sh` as your user.

To move to a newer devkitPro: put the new tag and its index digest in
`switch/devkitpro-image`, build, test, commit. `NATIVE=1` uses a devkitPro
installed on the host instead (`DEVKITPRO`, default `/opt/devkitpro`).
**Vulkan build (experimental).** `make switch-vk` builds the Vulkan
renderer (`nv2a_vk`) instead of OpenGL, as `<sd folder>-<edition>-vulkan.nro`,
in its own image, `switch/devkitpro-nvk-image`: devkitA64 with Mesa 26 for
Horizon, whose NVK (`portlibs/switch/lib/libvulkan.a`) the NRO links
statically, with SDL2 and FFmpeg as in devkitPro's. glslang (shaders are
compiled at run time) is not in it: `make glslang-switch` builds it once
into `work-dub/deps/glslang-switch`. Test on the console only: Eden cannot
run NVK (its GPU work never completes there, as nfsu2-sw found). The Vulkan
renderer lacks the fixes the OpenGL one has had since 2026-10-15 (the 2D
blit, depth for the motion blur, the 16:9 fill, FXAA, UI upscaling, the
texture and streaming speed-ups): a first run shows whether NVK runs the
game and how fast, not a finished picture.

## Sending builds to the Switch

- **nxlink** (fastest): open hbmenu, press **Y** (netloader), then
  `nxlink -s work-<game>/switch_sd/switch/mcdub3-nx/mcdub3-nx-usa.nro` (or `make send`). `-s` keeps the
  connection open and streams the log to your terminal. The game data must
  already be on the card at `sdmc:/switch/mcdub3-nx/game/`.
- **SD card**: copy `work-<game>/switch_sd/switch/mcdub3-nx/` to the card.
- Start with **title takeover** (hold R while launching a game). Applet mode
  leaves too little memory.
- Or install an **NSP forwarder**: `make nsp` builds one per edition in
  `APP_EDITIONS` (config/<game>/app.env; `EDITION=usa make nsp` for one),
  `work-<game>/nsp/mcdub3-nx-<edition>-<version>.nsp`: a home-menu app that starts
  `sdmc:/switch/mcdub3-nx/mcdub3-nx-<edition>.nro` as an application (full
  memory). Each edition has its own title ID (`APP_TITLE_ID_<edition>`), so
  editions install side by side; reinstalling one replaces it. It is
  built the way [NTON](https://github.com/rlaphoenix/NTON) builds forwarders:
  NTON's forwarder program (nx-hbloader + Forwarder-Mod, firmware 21+)
  reading the NRO path from its romfs, NTON's NACP settings, the icon
  re-encoded as a bare JPEG, and a start-up logo, packed by hacBrewPack with
  your console's keys (`keys/prod.keys`, never committed). NTON and
  hacBrewPack are fetched at pinned commits; nothing of theirs is in this
  repo. Name, version, title ID, artwork (`APP_ICON`: square, 1024x1024
  best; made into a 256x256 baseline JPEG for hbmenu and the home menu, an
  .icns for the Mac app and an .ico for Windows) and logo folder come from
  `config/<game>/app.env`, the author from `switch/app.env`. NSPs are built
  here only, never in CI: the keys stay on your machine (ADR 0008). The start-up logo
  covers the launch: put `NintendoLogo.png` (256x128) and
  `StartupMovie.gif` (256x80) in `assets/logo/` (personal, gitignored), or
  NTON's are used. The NSP holds only the forwarder: a new game build needs
  only a new NRO.

## Releases (Switch)

Official builds are Switch NROs, one per edition of each game (the DUB
Edition: usa, eur; the Remix: eur, usa), published together as GitHub
releases ([ADR 0005](docs/decisions/0005-switch-releases.md)).

| Stage | Workflow | Runs on | Started by |
|---|---|---|---|
| Tests | `ci.yml` | GitHub | every push and pull request; first stage of a build |
| Build | `switch-build.yml` | the self-hosted runner with the disc | you: Actions > Switch build > Run workflow |
| Release | `switch-release.yml` | GitHub (`release` environment: approval, then publish the NROs) | you: Actions > Switch release > Run workflow |

**To release a version**

1. Set `APP_VERSION` in `config/dub/app.env` and `config/remix/app.env`
   (e.g. `0.2.0`; the same in both: one release carries every game, and CI
   checks it).
2. Write its notes in `RELEASE_NOTES.md` under `## v0.2.0`. Commit and push.
3. Actions > **Switch build** > Run workflow, with an optional description.
   It runs the tests, then builds an NRO for every edition of every game
   on the runner (`APP_EDITIONS` in each `config/<game>/app.env`):
   `mcdub3-nx-<edition>.nro`, `mcdub3remix-nx-<edition>.nro`.
4. Try the NRO from the run's artifact on the console.
5. Actions > **Switch release** > Run workflow (pre-release or not). It
   releases the latest successful build; give a run ID only to release an
   older one. A build made before a change to `.github/workflows/` cannot be
   released: GitHub does not let a workflow's own token tag such a commit,
   and the release stops at once saying so. Build again, or add a
   `RELEASE_TOKEN` secret (a fine-grained token for this repository with
   Contents and Workflows: read and write) to release any build. A reviewer approves the `release` environment, and the
   workflow tags `v0.2.0` on the built commit and publishes the NROs with
   the notes. NSP forwarders are not part of it: build them locally with
   `make nsp` and attach them if you want (`gh release upload v0.2.0
   work-dub/nsp/*.nsp`).

**One-time set-up**

- *Approval gate*: Settings > Environments > New environment `release` >
  Required reviewers (yourself and anyone else who may approve). Optionally
  limit it to the `main` branch.
- *Fork pull requests*: Settings > Actions > General > "Require approval for
  all outside collaborators". The build workflow only starts by hand, so a
  pull request cannot reach the runner, but keep this on for a public repo.
- *The runner* (the Linux VM, with Docker as for `make switch`): Settings >
  Actions > Runners > New self-hosted runner > Linux, ARM64; follow the
  commands, and give it the extra label `mcdub3-disc`.
  In the runner's folder, `.env` sets where the discs are: one folder per
  game holding the disc its lift was made from (`MC3_EDITION` in
  `config/<game>/xbe_facts.h`); the other editions are built from it.

  ```sh
  # actions-runner/.env
  MC3_DISCS=/home/<you>/mcdub3-discs
  #   usa/default.xbe         the DUB Edition (lifted from the USA disc)
  #   remix-eur/default.xbe   the Remix (lifted from the European disc)
  ```

  The console keys never go to a runner (CLAUDE.md, Rules): NSPs are built
  locally. A runner set up earlier with the `mcdub3-keys` label and
  `MC3_KEYS` no longer needs either.

  Run it as a service (`sudo ./svc.sh install && sudo ./svc.sh start`). Each
  build lifts the disc again in a work folder beside the checkout
  (`mcdub3-work-<game>-<edition>`, kept between runs: Python venv, build
  trees; the older `mcdub3-work-<edition>` folders can be deleted).
- *Editions*: `config/<game>/xbe_facts.h` names the disc this lift was made from
  (`MC3_EDITION`, from the XBE's region), `config/<game>/xbe_editions.h` every
  edition whose code is the same (usa, eur); the NRO refuses any other
  `default.xbe` with a message. The build matrix builds one NRO per edition
  of each game (`make switch GAME=<game> EDITION=<edition>`:
  <sd folder>-<edition>.nro, looking in `game-<edition>/` on the card
  first, then `game/`), all from the lifted disc, so the runner needs only
  that one per game in `MC3_DISCS`. Editions whose code differs in branch
  conditions only share the lift too (edition variants, see Two games);
  other code differences would need a lift of their own (ADR 0005). Each edition's NSP
  has its own title ID and name in `config/<game>/app.env`.

## Texture packs (HD)

Replacement textures live in `hd/` beside `game/`, never inside it: the
user's disc stays untouched, and deleting `hd/` brings the original look
back. A texture in `hd/textures/<hash>.png` replaces the one with that
content hash, at any size (the renderer adds mip levels); see
`xboxrecomp/src/nv2a_gl/hd_pack.c`. `hd/` is gitignored: packs made from the
game's own textures are the game's art, for your own use.

Making or growing a pack:

1. **Dump** (VM): `make hd-dump RECOMP_GL_SCALE=1.5` and play the scenes you
   want. Every texture drawn is written once to `hd/dump/<hash>.png`; later
   dumps only add new ones.
2. **Upscale** (Mac: Real-ESRGAN has no ARM64 Linux build):
   `make hd-tools` once, then `make hd-upscale` (FACTOR=2|3|4, MIN=<px>,
   FILES="..." to redo some). Only textures not yet in `hd/textures/` are
   done, so hand-edited ones are kept. Colour goes through the AI upscaler;
   alpha is only resized, because MC3 keeps data there (gloss, window glow).
3. **Linux**: `make run` uses the pack at once (log: `[GL] HD pack ...: N
   replacement textures`).
4. **Switch**: `make switch` builds the plain version (no pack staged, and a
   previously staged one removed); `make switch HD=1` stages `hd/textures/`
   to `work-<game>/switch_sd/switch/mcdub3-nx/hd/textures/`. Copy that folder to
   the card, or delete `sdmc:/switch/mcdub3-nx/hd/` there to go back.
