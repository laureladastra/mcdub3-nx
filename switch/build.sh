#!/usr/bin/env bash
# Build the Switch NRO and stage an SD-card layout:
#
#   <SD>/switch/mcdub3-nx/mcdub3-nx-<edition>.nro   the edition of the lift
#                                             (config/<game>/xbe_facts.h: usa, ...);
#                                             -vulkan / -lto added for those
#   (GAME=remix: <SD>/switch/mcdub3remix-nx/mcdub3remix-nx-<edition>.nro,
#   mcdub3remix_env.txt, ...: config/remix/game.env)
#   <SD>/switch/mcdub3-nx/game/               the extracted disc (never the ISO)
#   <SD>/switch/mcdub3-nx/mcdub3_env.txt      optional KEY=VALUE runtime switches
#   <SD>/switch/mcdub3-nx/hd/textures/        the HD texture pack, with HD=1 only
#   <SD>/switch/mcdub3-nx/mcdub3_log.txt      written when no nxlink host listens
#
# Environment (defaults from tools/env.sh):
#   DEVKITPRO      /opt/devkitpro (switch-dev, switch-sdl2, switch-mesa)
#   MC3_GEN_DIR    lifted C from tools/regen.sh
#   SD_ROOT        staging SD root (default $MC3_WORK/switch_sd)
#   BUILD_DIR      default $MC3_WORK/build-switch[-vk][-lto]
#   JOBS           parallel compile jobs (default: by free memory)
#   VULKAN=1       Vulkan renderer against NVK_SDK / GLSLANG_DIR
#                  (default $MC3_WORK/deps/{mesa-sdk/usr/local,glslang-switch})
#   LTO=1          link-time optimisation (LTO_JOBS, ~2 GB each)
#   NO_GAME=1      stage only the NRO (skip syncing the disc)
#
# Then: copy <SD>/switch/mcdub3-nx/ to the card, or `nxlink -s` the NRO, and start
# it from hbmenu with title takeover (hold R on a game) for full RAM.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../tools/env.sh"
mc3_check_lift_fresh

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
SD="${SD_ROOT:-$MC3_WORK/switch_sd}"
# The NRO is named for its edition (mcdub3-nx-<edition>.nro), as releases
# and the NSP forwarders name it: EDITION=eur, else the lifted one
# (MC3_EDITION, written by tools/first_lift.sh). Every edition in
# config/<game>/xbe_editions.h runs on this lift; the edition only names the
# package and the disc folder it looks in first (game-<edition>/).
LIFT_EDITION=$(mc3_lift_edition)
[ -n "$LIFT_EDITION" ] || mc3_fail "config/$MC3_GAME/xbe_facts.h has no MC3_EDITION: make lift GAME=$MC3_GAME"
EDITION="${EDITION:-$LIFT_EDITION}"
KNOWN=$(sed -n 's/.*X("\([a-z0-9_-]*\)".*/\1/p' "$MC3_CONFIG/xbe_editions.h" | tr '\n' ' ')
case " $KNOWN " in *" $EDITION "*) ;; *) mc3_fail "EDITION=$EDITION is not in config/$MC3_GAME/xbe_editions.h (there: $KNOWN)";; esac
NRO_NAME="$MC3_SD_DIR-$EDITION"
SUFFIX=""
ARGS=(-DMC3_VULKAN=OFF)
if [ "${VULKAN:-0}" = "1" ]; then
    SUFFIX="-vk"; NRO_NAME="$NRO_NAME-vulkan"
    NVK_SDK="${NVK_SDK:-$MC3_WORK/deps/mesa-sdk/usr/local}"
    GLSLANG_DIR="${GLSLANG_DIR:-$MC3_WORK/deps/glslang-switch}"
    [ -d "$NVK_SDK" ] || mc3_fail "VULKAN=1 needs mesa-switch NVK in $NVK_SDK"
    ARGS=(-DMC3_VULKAN=ON -DNVK_SDK="$NVK_SDK" -Dglslang_DIR="$GLSLANG_DIR/lib/cmake/glslang")
fi
if [ "${LTO:-0}" = "1" ]; then
    SUFFIX="$SUFFIX-lto"; NRO_NAME="$NRO_NAME-lto"
    ARGS+=(-DMC3_LTO=ON -DMC3_LTO_JOBS="${LTO_JOBS:-4}")
fi
BUILD="${BUILD_DIR:-$MC3_WORK/build-switch$SUFFIX}"
# A work/ shared between machines (the Linux VM sees the repo at
# /media/psf/..., the Mac's container at /Users/...): CMake refuses a tree
# configured for another path, so each path gets its own (2026-10-24).
if [ -z "${BUILD_DIR:-}" ] && [ -f "$BUILD/CMakeCache.txt" ] \
        && ! grep -qx "CMAKE_HOME_DIRECTORY:INTERNAL=$MC3_REPO" "$BUILD/CMakeCache.txt"; then
    BUILD="$BUILD-$(printf %s "$MC3_REPO" | md5sum | cut -c1-6)"
    echo "  (${MC3_WORK##*/}/build-switch$SUFFIX belongs to another checkout path: building in ${BUILD##*/})"
fi
JOBS="${JOBS:-$(mc3_default_jobs)}"

[ -f "$DEVKITPRO/cmake/Switch.cmake" ] || mc3_fail "devkitPro not found at $DEVKITPRO (vm/setup-ubuntu.sh)"
[ -f "$DEVKITPRO/portlibs/switch/include/SDL2/SDL.h" ] || mc3_fail "switch-sdl2 missing"
[ -f "$MC3_GEN_DIR/recomp_funcs.h" ] || mc3_fail "no generated code in $MC3_GEN_DIR (tools/regen.sh)"

# The app's identity (name, author, version): config/<game>/app.env and
# switch/app.env (mc3_app_env); the edition's own name and artwork
# (APP_NAME_<edition>, APP_ICON_<edition>) if it has them. The icon is the
# artwork as hbmenu wants it (256x256 baseline JPEG, work-<game>/icons/),
# made on the host by make switch; none: hbmenu's default.
mc3_app_env
APP_NAME=$(mc3_app_var NAME "$EDITION")
ICON=$(mc3_switch_icon "$EDITION")
echo "  $APP_NAME $APP_VERSION ($EDITION), icon: $([ -n "$ICON" ] && echo "${ICON#$MC3_REPO/}" || echo none)"
cmake -S "$MC3_REPO" -B "$BUILD" -G Ninja \
    -DMC3_SWITCH_TITLE="$APP_NAME" -DMC3_SWITCH_AUTHOR="$APP_AUTHOR" \
    -DMC3_SWITCH_VERSION="$APP_VERSION" -DMC3_SWITCH_ICON="${ICON:-}" \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DXBOXRECOMP_DIR="$XBOXRECOMP_DIR" -DMC3_GEN_DIR="$MC3_GEN_DIR" \
    -DMC3_APP="$MC3_APP" -DMC3_SD_DIR="$MC3_SD_DIR" \
    -DMC3_VERSION="$APP_VERSION" -DMC3_BUILD_ID="${MC3_BUILD_ID:-local}" \
    -DMC3_PACKAGE_EDITION="$EDITION" "${ARGS[@]}" >/dev/null
ninja -C "$BUILD" -j"$JOBS"

DEST="$SD/switch/$MC3_SD_DIR"
mkdir -p "$DEST"
# The NRO and ELF carry the executable's name ($MC3_APP; mcdub3 before the
# games were split).
BUILT="$BUILD/$MC3_APP"
[ -f "$BUILT.nro" ] || BUILT="$BUILD/mcdub3"
cp "$BUILT.nro" "$DEST/$NRO_NAME.nro"
rm -f "$DEST/mcdub3.nro" "$DEST/mcdub3-vulkan.nro" "$DEST/mcdub3-lto.nro"   # names before 2026-10-21
# Keep the ELF of every staged NRO, by hash: crash offsets in a console log
# only resolve against the exact build that wrote it.
mkdir -p "$MC3_WORK/elf"
ELF_KEEP="$MC3_WORK/elf/$NRO_NAME-$(md5sum "$DEST/$NRO_NAME.nro" | cut -c1-8).elf"
cp "$BUILT.elf" "$ELF_KEEP"
# Release output (CI, .github/workflows/switch-build.yml): the NRO named
# for its edition, as players pick it (mcdub3-nx-usa.nro, later -pal-..),
# its ELF for crash offsets, and what was built.
if [ -n "${RELEASE_DIR:-}" ]; then
    mkdir -p "$RELEASE_DIR"
    cp "$DEST/$NRO_NAME.nro" "$RELEASE_DIR/$NRO_NAME.nro"
    cp "$BUILT.elf" "$RELEASE_DIR/$NRO_NAME.elf"
    {
        echo "version=$APP_VERSION"
        echo "game=$MC3_GAME"
        echo "title=$MC3_GAME_TITLE"
        echo "edition=$EDITION"
        echo "build=${MC3_BUILD_ID:-local}"
        echo "commit=${GITHUB_SHA:-$(git -C "$MC3_REPO" rev-parse HEAD 2>/dev/null || echo unknown)}"
        echo "nro_sha256=$(sha256sum "$RELEASE_DIR/$NRO_NAME.nro" | cut -d' ' -f1)"
    } > "$RELEASE_DIR/$NRO_NAME.buildinfo"
    echo "  release: $RELEASE_DIR/$NRO_NAME.nro (version $APP_VERSION)"
fi
if [ "${NO_GAME:-0}" != "1" ]; then
    mc3_need_game
    # The disc is gigabytes: hard-link it from $MC3_GAME_DIR instead of
    # copying (same disk, no extra space); only changed files are copied.
    # A disc from game-<edition>/ goes to game-<edition>/ on the card too
    # (each NRO looks in its own first), one from game/ to game/. The
    # Remix has its own SD folder, so its game-remix[-<edition>]/ goes to
    # game[-<edition>]/ in there.
    SD_GAME="${MC3_GAME_DIR##*/}"
    SD_GAME="game${SD_GAME#"$MC3_DISC_PREFIX"}"
    case "$SD_GAME" in game|game-*) ;; *) SD_GAME=game ;; esac
    rsync -a --size-only --link-dest="$MC3_GAME_DIR/" \
        "$MC3_GAME_DIR/" "$DEST/$SD_GAME/" --exclude '*_analysis.json'
fi
# The HD texture pack (hd/textures beside game/, see nv2a_gl/hd_pack.c):
# HD=1 stages it next to the game, otherwise a pack staged by an earlier
# HD=1 build is removed, so the staged folder matches the choice. Only
# textures/ goes: hd/dump is raw material for tools/hd_upscale.sh.
if [ "${HD:-0}" = "1" ]; then
    [ -d "$MC3_REPO/hd/textures" ] || mc3_fail "HD=1 but no hd/textures: make hd-dump, then make hd-upscale"
    mkdir -p "$DEST/hd/textures"
    rsync -a --delete "$MC3_REPO/hd/textures/" "$DEST/hd/textures/"
    echo "  HD pack: $(find "$DEST/hd/textures" -name '*.png' | wc -l) textures staged ($(du -sh "$DEST/hd" | cut -f1))"
else
    rm -rf "$DEST/hd"
fi
# Runtime switches for the console, staged with the NRO: your own
# switch/<app>_env.txt (gitignored) if there is one, else the documented
# template with every setting commented out (switch/<app>_env.example.txt,
# else the DUB Edition's: the settings are the same). The template also
# goes along as <app>_env.example.txt, the reference.
TEMPLATE="$MC3_REPO/switch/${MC3_APP}_env.example.txt"
[ -f "$TEMPLATE" ] || TEMPLATE="$MC3_REPO/switch/mcdub3_env.example.txt"
if [ -f "$MC3_REPO/switch/${MC3_APP}_env.txt" ]; then
    cp "$MC3_REPO/switch/${MC3_APP}_env.txt" "$DEST/${MC3_APP}_env.txt"
    echo "  settings: switch/${MC3_APP}_env.txt staged"
elif [ -f "$TEMPLATE" ]; then
    cp "$TEMPLATE" "$DEST/${MC3_APP}_env.txt"
    echo "  settings: the template staged (all defaults; copy it to switch/${MC3_APP}_env.txt to keep your own)"
fi
[ -f "$TEMPLATE" ] && cp "$TEMPLATE" "$DEST/${MC3_APP}_env.example.txt"
echo
echo "staged $DEST"
echo "  $NRO_NAME.nro $(stat -c %s "$DEST/$NRO_NAME.nro") bytes, ELF kept as $ELF_KEEP"
echo "  send it:  nxlink -s $DEST/$NRO_NAME.nro   (hbmenu open, Y = netloader)"
