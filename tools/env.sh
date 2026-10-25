# Shared defaults for the scripts in tools/, switch/ and vm/. Sourced, not run.
#
# Every path is relative to the repo root, found from this file's location, so
# the scripts work from anywhere (e.g. `bash tools/first_lift.sh` in the root).
#
# Two games share the repo (ADR 0006): GAME=dub (Midnight Club 3: DUB
# Edition, the default) and GAME=remix (DUB Edition Remix). Per game:
#
#   config/<game>/         xbe_facts.h, xbe_editions.h, seed_functions.json,
#                          game.env (names, disc folders)
#   src/games/<game>/      recomp_manual.c: the overrides, at its addresses
#   game[-remix][-<ed>]/   the extracted disc (gitignored)
#   work-<game>/           everything generated (gitignored):
#       ├── analysis/      xbe_parser / disasm / func_id / abi_analysis output
#       ├── gen/           lifted C from tools/regen.sh
#       ├── build-linux/, build-mac/, build-switch/ ...
#       ├── switch_sd/     SD-card staging
#       ├── logs/, elf/
#       ├── venv/          Python + capstone for the lift pipeline
#       └── deps/          Switch-side extras (mesa NVK SDK, glslang, FFmpeg)
#
# Overrides, all optional: MC3_GAME_DIR (disc), MC3_WORK (generated files),
# MC3_SEEDS, MC3_MANUAL, MC3_FACTS_H.

MC3_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export MC3_GAME="${MC3_GAME:-${GAME:-dub}}"
export MC3_CONFIG="$MC3_REPO/config/$MC3_GAME"
[ -f "$MC3_CONFIG/game.env" ] || { echo "error: unknown game '$MC3_GAME' (GAME=dub or GAME=remix)" >&2; exit 1; }
# shellcheck source=/dev/null
. "$MC3_CONFIG/game.env"
export MC3_GAME_TITLE MC3_SHORT_NAME MC3_DISC_PREFIX
export MC3_SEEDS="${MC3_SEEDS:-$MC3_CONFIG/seed_functions.json}"
export MC3_MANUAL="${MC3_MANUAL:-$MC3_REPO/src/games/$MC3_GAME/recomp_manual.c}"
export MC3_FACTS_H="${MC3_FACTS_H:-$MC3_CONFIG/xbe_facts.h}"
# work/ was the DUB Edition's before the games were split (2026-10-24):
# moved once to work-dub/ (build trees reconfigure themselves).
if [ -z "${MC3_WORK:-}" ] && [ "$MC3_GAME" = dub ] && [ -d "$MC3_REPO/work" ] && [ ! -e "$MC3_REPO/work-dub" ]; then
    mv "$MC3_REPO/work" "$MC3_REPO/work-dub" && echo "note: work/ moved to work-dub/ (one work folder per game now)" >&2
fi
export MC3_WORK="${MC3_WORK:-$MC3_REPO/work-$MC3_GAME}"
export XBOXRECOMP_DIR="${XBOXRECOMP_DIR:-$MC3_REPO/xboxrecomp}"
# The disc. Several editions can sit side by side: $MC3_DISC_PREFIX/ and
# $MC3_DISC_PREFIX-<edition>/ -- game/, game-eur/ for the DUB Edition,
# game-remix/, game-remix-eur/ for the Remix (config/<game>/xbe_editions.h;
# one lift runs them all). In this order:
#   1. MC3_GAME_DIR, if set;
#   2. EDITION=<name>: <prefix>-<name>/ (else <prefix>/: for the DUB Edition
#      only with usa, the edition game/ held before editions);
#   3. the only disc of this game there is;
#   4. of several: <prefix>/, else the lifted edition's (xbe_facts.h), else
#      the first by name -- said once, with EDITION= to pick another.
mc3_lift_edition() {
    sed -n 's/^#define MC3_EDITION *"\(.*\)"/\1/p' "$MC3_FACTS_H" 2>/dev/null
}
mc3_pick_game_dir() {
    local d discs=() lift p="$MC3_REPO/$MC3_DISC_PREFIX"
    if [ -n "${EDITION:-}" ]; then
        MC3_GAME_DIR="$p-$EDITION"
        if [ ! -e "$MC3_GAME_DIR/default.xbe" ] && [ -e "$p/default.xbe" ] \
           && { [ "$MC3_GAME" != dub ] || [ "$EDITION" = usa ]; }; then
            MC3_GAME_DIR="$p"
        fi
        return
    fi
    for d in "$p" "$p"-*; do
        # game-* holds the Remix's folders too: not the DUB Edition's.
        case "${d##*/}" in game-remix*) [ "$MC3_GAME" = dub ] && continue ;; esac
        [ -e "$d/default.xbe" ] && discs+=("$d")
    done
    case ${#discs[@]} in
        0) MC3_GAME_DIR="$p"; return ;;                  # mc3_need_game explains
        1) MC3_GAME_DIR="${discs[0]}"; return ;;
    esac
    lift=$(mc3_lift_edition)
    if [ -e "$p/default.xbe" ]; then
        MC3_GAME_DIR="$p"
    elif [ -n "$lift" ] && [ -e "$p-$lift/default.xbe" ]; then
        MC3_GAME_DIR="$p-$lift"
    else
        MC3_GAME_DIR="${discs[0]}"
    fi
    if [ -z "${MC3_DISC_NOTED:-}" ]; then
        local names=() n
        for d in "${discs[@]}"; do names+=("${d##*/}"); done
        n="${MC3_GAME_DIR##*/}"
        echo "note: several discs (${names[*]}): $n is used; EDITION=<name> picks another ($MC3_DISC_PREFIX-<name>/)" >&2
        export MC3_DISC_NOTED=1                          # once per make, not per script
    fi
}
[ -n "${MC3_GAME_DIR:-}" ] || mc3_pick_game_dir
export MC3_GAME_DIR
export MC3_XBE="${MC3_XBE:-$MC3_GAME_DIR/default.xbe}"
export MC3_ANALYSIS_DIR="${MC3_ANALYSIS_DIR:-$MC3_WORK/analysis}"
export MC3_GEN_DIR="${MC3_GEN_DIR:-$MC3_WORK/gen}"

# The desktop host: Linux (the VM) or macOS (native, Apple Silicon). Each
# builds into its own tree under work-<game>/ (build-linux*, build-mac*), so the
# VM and the Mac can share the workspace folder.
case "$(uname -s)" in
    Darwin) MC3_HOST=mac ;;
    MINGW*|MSYS*|CYGWIN*) MC3_HOST=windows ;;   # MSYS2 (tools/setup_windows.sh)
    *) MC3_HOST=linux ;;
esac
export MC3_HOST
# Windows: the MSYS2 environment decides the architecture (CLANG64 = x86-64,
# CLANGARM64 = ARM64); builds go to work-<game>/build-windows-<arch>/.
if [ "$MC3_HOST" = windows ]; then
    case "${MSYSTEM:-}" in
        CLANGARM64) export MC3_WIN_ARCH=aarch64 ;;
        CLANG64) export MC3_WIN_ARCH=x86_64 ;;
        *) echo "error: run this in MSYS2's CLANG64 or CLANGARM64 shell (install_windows.bat does)" >&2; exit 1 ;;
    esac
fi

# macOS: build and run as arm64 with the Apple Silicon Homebrew
# (/opt/homebrew). A shell under Rosetta (uname -m = x86_64) would pick the
# Intel Homebrew in /usr/local and build an x86 binary; re-run natively.
# Call at the top of a script: mc3_mac_native "$0" "$@".
mc3_mac_native() {
    [ "$MC3_HOST" = mac ] || return 0
    if [ "$(uname -m)" != arm64 ] && [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" = 1 ]; then
        exec arch -arm64 /bin/bash "$@"
    fi
    [ -d /opt/homebrew/bin ] && export PATH="/opt/homebrew/bin:$PATH"
    return 0
}

# The lift pipeline's Python: the first that can import capstone, from the
# project venv (vm/setup-ubuntu.sh) or the system. Scripts call mc3_python.
mc3_python() {
    local py
    # venv: Linux (vm/setup-ubuntu.sh); macvenv: macOS (tools/setup_mac.sh),
    # in this game's work folder or the DUB Edition's (one setup serves both).
    # A shared work folder may hold both.
    for py in "$MC3_WORK/venv/bin/python3" "$MC3_WORK/macvenv/bin/python3" \
              "$MC3_REPO/work-dub/venv/bin/python3" "$MC3_REPO/work-dub/macvenv/bin/python3" python3; do
        if "$py" -c 'import capstone' >/dev/null 2>&1; then
            MC3_PYTHON="$py"
            return 0
        fi
    done
    echo "error: no Python with capstone (the x86 disassembler the lift needs)" >&2
    echo "  - run the setup, which creates the venv:      make setup" >&2
    echo "  - or install it for your user:                python3 -m pip install --user capstone" >&2
    exit 1
}

# The Switch app: files named $MC3_APP (.nro, _env.txt, _log.txt) in
# sdmc:/switch/$MC3_SD_DIR/ on the card.
export MC3_APP="${MC3_APP:-$MC3_APP_DEFAULT}"
export MC3_SD_DIR="${MC3_SD_DIR:-$MC3_SD_DIR_DEFAULT}"

# The app's identity (name, version, editions, title IDs, artwork):
# switch/app.env (what the games share: author, pinned tools), then
# config/<game>/app.env. Sets the APP_* variables in the caller.
mc3_app_env() {
    # shellcheck disable=SC1091
    source "$MC3_REPO/switch/app.env"
    [ -f "$MC3_CONFIG/app.env" ] || mc3_fail "no config/$MC3_GAME/app.env"
    # shellcheck disable=SC1091
    source "$MC3_CONFIG/app.env"
}

# An edition's APP_<KEY>_<edition> (a '-' of the edition as '_'), else
# APP_<KEY>: mc3_app_var NAME eur.
mc3_app_var() {
    local v="APP_${1}_${2//-/_}" d="APP_$1"
    printf '%s' "${!v:-${!d:-}}"
}

# The artwork for an edition, as an absolute path; empty when there is none
# (a default icon then).
mc3_app_artwork() {
    local p
    p=$(mc3_app_var ICON "${1:-}")
    [ -n "$p" ] && [ -f "$MC3_REPO/$p" ] && printf '%s' "$MC3_REPO/$p"
    return 0
}

# A Python with Pillow, for the icons: the project venvs, the system's, or
# one of its own in work-<game>/pillow-venv (system Pythons refuse pip
# installs: PEP 668). Prints its path; empty if none can be had.
mc3_pillow_python() {
    local c
    for c in "$MC3_WORK/venv/bin/python3" "$MC3_WORK/macvenv/bin/python3" \
             "$MC3_REPO/work-dub/venv/bin/python3" "$MC3_REPO/work-dub/macvenv/bin/python3" \
             "$MC3_WORK/pillow-venv/bin/python3" python3; do
        if "$c" -c 'import PIL' >/dev/null 2>&1; then printf '%s' "$c"; return 0; fi
    done
    if python3 -m venv "$MC3_WORK/pillow-venv" >/dev/null 2>&1 \
            && "$MC3_WORK/pillow-venv/bin/pip" install --quiet pillow >/dev/null 2>&1; then
        printf '%s' "$MC3_WORK/pillow-venv/bin/python3"
    fi
    return 0
}

# The Switch icon of an edition (hbmenu and the home menu): a bare 256x256
# baseline JPEG made from the artwork into work-<game>/icons/, on the host
# (the container has no Pillow: make switch makes it there first). Prints
# its path; empty without artwork or Pillow (a default icon then).
mc3_switch_icon() {
    local edition=$1 art out py
    art=$(mc3_app_artwork "$edition")
    [ -n "$art" ] || return 0
    out="$MC3_WORK/icons/switch-$edition.jpg"
    if [ ! -f "$out" ] || [ "$art" -nt "$out" ] || [ "$MC3_REPO/tools/app_icon.py" -nt "$out" ]; then
        mkdir -p "$MC3_WORK/icons"
        py=$(mc3_pillow_python)
        if [ -z "$py" ] || ! "$py" "$MC3_REPO/tools/app_icon.py" switch "$art" "$out" >&2; then
            echo "  (no icon: no Python with Pillow here; python3 -m pip install pillow)" >&2
            rm -f "$out"
            return 0
        fi
    fi
    printf '%s' "$out"
}

# Parallel compile jobs: ~1.5 GB of RAM per job on the generated chunks.
mc3_default_jobs() {
    local mem_mb cpus jobs
    if [ -r /proc/meminfo ]; then
        # MemAvailable on Linux; MSYS2's /proc/meminfo has only MemFree and
        # MemTotal (an empty value broke the arithmetic, 2026-10-23).
        mem_mb=$(awk '/^MemAvailable:/ {a=$2} /^MemFree:/ {f=$2} /^MemTotal:/ {t=$2}
                      END {print int((a ? a : (f ? f : t / 2)) / 1024)}' /proc/meminfo)
    else
        mem_mb=$(( $(sysctl -n hw.memsize) / 1048576 / 2 ))
    fi
    [ -n "$mem_mb" ] || mem_mb=3000
    cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 2)
    jobs=$(( mem_mb / 1500 ))
    [ "$jobs" -lt 1 ] && jobs=1
    [ "$jobs" -gt "$cpus" ] && jobs=$cpus
    echo "$jobs"
}

mc3_fail() { echo "error: $*" >&2; exit 1; }

# Which lifted functions the game's recomp_manual.c replaces or wraps, as the lifter
# sees it: the toolkit's own scanner (tools/recomp/manual_scan.py), which
# skips `#if 0` blocks and counts `extern void sub_X_gen(void);` as a wrap.
# regen.sh records this list next to the lifted C; the build compares.
mc3_manual_overrides() {
    local py
    for py in "$MC3_WORK/venv/bin/python3" "$MC3_WORK/macvenv/bin/python3" \
              "$MC3_REPO/work-dub/venv/bin/python3" "$MC3_REPO/work-dub/macvenv/bin/python3" python3; do
        (cd "$XBOXRECOMP_DIR" && "$py" - "$MC3_MANUAL" "$MC3_GEN_DIR/zz_trace_wrappers.c" <<'PY'
import os, sys
from tools.recomp.manual_scan import scan
replaced, wrapped = set(), set()
for path in sys.argv[1:]:          # the overrides, and the trace wrappers if lifted
    if os.path.exists(path):
        r, w = scan(path)[:2]
        replaced |= set(r)
        wrapped |= set(w)
for a in sorted(replaced): print("replaces sub_%08X" % a)
for a in sorted(wrapped):  print("wraps    sub_%08X" % a)
PY
        ) 2>/dev/null && return 0
    done
    return 1
}

# Stop a build of lifted C that predates its inputs:
#  - new seeds only take effect after a full regen;
#  - adding or removing an override in src/games/<game>/recomp_manual.c needs a relift, so
#    the lifter drops (or restores) the lifted body it replaces -- otherwise
#    "multiple definition of sub_XXXXXXXX" at link time. Editing the body of an
#    existing override, or any other C, is just a build.
# MC3_SKIP_LIFT_CHECK=1 skips this.
mc3_check_lift_fresh() {
    local gen="$MC3_GEN_DIR/recomp_funcs.h" stamp="$MC3_GEN_DIR/.manual_overrides"
    [ "${MC3_SKIP_LIFT_CHECK:-0}" = "1" ] && return 0
    [ -f "$gen" ] || mc3_fail "no lifted C in $MC3_GEN_DIR: run make lift"
    if [ "$MC3_SEEDS" -nt "$gen" ]; then
        mc3_fail "${MC3_SEEDS#$MC3_REPO/} is newer than the lifted C: run make regen first (make regen build)"
    fi
    # The stamp every lift writes (regen.sh): recomp_funcs.h keeps its date
    # when a relift leaves it unchanged.
    local lifted="$gen"
    [ -f "$stamp" ] && lifted="$stamp"
    if [ "$MC3_CONFIG/not_functions.json" -nt "$lifted" ]; then
        mc3_fail "config/$MC3_GAME/not_functions.json is newer than the lifted C: run make regen first (make regen build)"
    fi
    if [ "$MC3_CONFIG/edition_variants.json" -nt "$lifted" ]; then
        mc3_fail "config/$MC3_GAME/edition_variants.json is newer than the lifted C: run make relift first (make relift build)"
    fi
    local now
    if ! now=$(mc3_manual_overrides | tr -d '\r'); then
        echo "warning: no Python to scan ${MC3_MANUAL#$MC3_REPO/}; override check skipped" >&2
        return 0
    fi
    # Without CRs: Windows' Python writes CRLF, and a work folder shared with a
    # Windows VM then failed the check both ways (2026-10-23).
    if [ "$now" != "$(tr -d '\r' < "$stamp" 2>/dev/null)" ]; then
        echo "error: the overrides in ${MC3_MANUAL#$MC3_REPO/} differ from the last lift:" >&2
        diff <(tr -d '\r' < "$stamp" 2>/dev/null) <(echo "$now") | grep '^[<>]' \
            | sed 's/^</  gone since the lift: /; s/^>/  new since the lift:  /' >&2
        mc3_fail "run make relift first (make relift build)"
    fi
}

# Fail with the ways to get the extracted disc where the scripts look for it.
mc3_need_game() {
    [ -f "$MC3_GAME_DIR/default.xbe" ] && return 0
    echo "error: no default.xbe in $MC3_GAME_DIR" >&2
    [ -n "${EDITION:-}" ] && echo "  (EDITION=$EDITION: the disc goes in $MC3_DISC_PREFIX-$EDITION/, e.g. make extract ISO=<file.iso> GAME=$MC3_GAME EDITION=$EDITION)" >&2
    echo "  Put the extracted disc (default.xbe + data) directly in the repo's $MC3_DISC_PREFIX/ folder." >&2
    echo "  - from an ISO:              tools/extract_disc.sh <file.iso>" >&2
    echo "  - or use it in place:       export MC3_GAME_DIR=<folder with default.xbe>" >&2
    exit 1
}
