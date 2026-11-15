#!/usr/bin/env bash
# One-time setup of the Ubuntu (ARM64) development VM: everything needed to
# lift, build and debug on Linux, and to cross-build the Switch NRO.
#
#   bash vm/setup-ubuntu.sh
#
# Safe to re-run. Needs sudo. See CONTRIBUTING.md for creating the VM.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../tools/env.sh"

echo "== 1/4 runtime build deps (the toolkit's own list: SDL2, GL/epoxy, OpenSSL, ...)"
bash "$XBOXRECOMP_DIR/tools/linux/install_deps.sh"

echo "== 2/4 tools: ninja, gdb, Vulkan (lavapipe for headless runs), rsync, python venv"
sudo pacman -S --needed ninja gdb rsync python-pip \
    vulkan-devel vulkan-radeon glslang \
    spirv-tools xorg-server-xvfb mesa-utils ffmpeg

echo "   perf for profiling (make profile), and permission to profile own processes"
sudo pacman -S --needed "linux-tools-$(uname -r)" linux-tools-generic || \
    echo "   (no linux-tools for kernel $(uname -r); make profile will say so)"
echo 'kernel.perf_event_paranoid=1' | sudo tee /etc/sysctl.d/60-mc3-perf.conf >/dev/null
sudo sysctl -q -p /etc/sysctl.d/60-mc3-perf.conf || true

echo "== 3/4 Python venv with capstone for the lift pipeline ($MC3_WORK/venv)"
mkdir -p "$MC3_WORK"
# --copies: work/ is on the shared folder, which may not take symlinks.
[ -x "$MC3_WORK/venv/bin/python3" ] || python -m venv --copies "$MC3_WORK/venv"
# Through the venv's python, not bin/pip: pip's script carries the venv's
# absolute path in its first line and breaks when the repo folder is renamed.
"$MC3_WORK/venv/bin/python" -m pip install -q --upgrade pip capstone pytest numpy unicorn
# No shell profile changes: tools/env.sh puts work/venv on PATH for the scripts.

echo "== 4/4 Docker, for the Switch build in devkitPro's container (switch/devkitpro-image)"
# The Switch toolchain is not installed on this machine: make switch runs in
# devkitPro's pinned image, the same one CI uses (switch/docker_build.sh).
sudo pacman -S --needed -y docker docker-compose docker-buildx
sudo usermod -aG docker "$USER"      # takes effect at the next login
sudo docker pull "$(grep -v '^#' "$(dirname "$0")/../switch/devkitpro-image" | head -1)"

echo
echo "done. Open a new shell (or: source ~/.bashrc), then:"
echo "  tools/extract_disc.sh <iso>   ->  tools/first_lift.sh  ->  tools/build_linux.sh"
