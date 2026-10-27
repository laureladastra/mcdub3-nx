#!/usr/bin/env bash
# The Switch build, inside devkitPro's own container image.
#
#   switch/docker_build.sh [script]   (make switch / make nsp; NATIVE=1 skips this)
#                                     script: default switch/build.sh
#
# Why a container: one toolchain everywhere -- this VM, another developer's
# machine, GitHub Actions -- pinned by digest in switch/devkitpro-image, with
# nothing installed or patched on the host. devkitPro publishes the image for
# arm64 and amd64; it has devkitA64, libnx, the Switch portlibs and the build
# tools switch/build.sh uses (cmake, ninja, python3, rsync). Also, devkitpro.org
# answers 403 from some networks (2026-10-16), Docker Hub does not.
#
# The repository is mounted at the same path, so paths in logs and in
# work/build-switch are the host's; the build runs as the calling user, so
# nothing in work/ ends up owned by root. Environment as for switch/build.sh.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../tools/env.sh"

IMAGE=$(grep -v '^#' "$MC3_REPO/switch/devkitpro-image" | head -1)
command -v docker >/dev/null || mc3_fail "no docker: sudo apt install docker.io (or make setup), or NATIVE=1 with devkitPro installed"
DOCKER=(docker)
if ! docker info >/dev/null 2>&1; then
    # No daemon to talk to. On a Mac that is Docker Desktop not running (a
    # CI runner then failed on "sudo: a terminal is required", 2026-10-24);
    # on Linux it can be a user not yet in the docker group, where sudo
    # helps -- but only with a terminal to ask for the password.
    case "$(uname -s)" in
        Darwin) mc3_fail "Docker is not running: start Docker Desktop (and enable 'Start Docker Desktop when you sign in' for a CI runner)" ;;
    esac
    [ -t 0 ] || mc3_fail "cannot reach the Docker daemon (is it running? is this user in the docker group?), and no terminal for sudo"
    DOCKER=(sudo docker)                                # not in the docker group yet
fi

ENVS=()
SCRIPT="${1:-switch/build.sh}"
for v in MC3_GAME MC3_WORK MC3_GAME_DIR MC3_GEN_DIR MC3_APP MC3_SD_DIR SD_ROOT BUILD_DIR JOBS VULKAN LTO LTO_JOBS NO_GAME KEYS HD \
         RELEASE_DIR MC3_BUILD_ID GITHUB_SHA EDITION; do
    [ -n "${!v:-}" ] && ENVS+=(-e "$v=${!v}")
done

# Folders outside the repo (a CI runner keeps MC3_WORK, the disc, the
# release output and the keys elsewhere) are mounted at the same paths.
MOUNTS=(-v "$MC3_REPO:$MC3_REPO")
for d in "$MC3_WORK" "$MC3_GAME_DIR" "${RELEASE_DIR:-}" "$([ -n "${KEYS:-}" ] && dirname "$KEYS")"; do
    [ -n "$d" ] || continue
    mkdir -p "$d"
    case "$(cd "$d" && pwd -P)/" in "$(cd "$MC3_REPO" && pwd -P)/"*) ;; *) MOUNTS+=(-v "$d:$d") ;; esac
done

# The icons, here on the host: the container has no Pillow (the artwork as
# a 256x256 baseline JPEG per edition, work-<game>/icons/; mc3_switch_icon).
mc3_app_env
for ed in ${EDITION:-$APP_EDITIONS}; do
    mc3_switch_icon "$ed" >/dev/null
done

echo "$SCRIPT in $IMAGE"
exec "${DOCKER[@]}" run --rm \
    -u "$(id -u):$(id -g)" -e HOME=/tmp \
    "${MOUNTS[@]}" -w "$MC3_REPO" \
    "${ENVS[@]}" \
    "$IMAGE" bash "$SCRIPT"
