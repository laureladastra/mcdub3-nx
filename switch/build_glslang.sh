#!/usr/bin/env bash
# glslang for the Switch, once, for the Vulkan build (make switch-vk): the
# renderer compiles its shaders to SPIR-V at run time with it, and the NVK
# image (switch/devkitpro-nvk-image) does not carry it.
#
#   make glslang-switch      fetch on the host, build in the NVK image
#
# Static libraries and CMake config into work-dub/deps/glslang-switch (both
# games use it). No optimizer (ENABLE_OPT=OFF: no SPIRV-Tools), no HLSL,
# no tools. GLSLANG_REF pins the source.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../tools/env.sh"
GLSLANG_REPO="https://github.com/KhronosGroup/glslang"
GLSLANG_REF="${GLSLANG_REF:-15.4.0}"
SRC="$MC3_REPO/work-dub/deps/src/glslang"
OUT="$MC3_REPO/work-dub/deps/glslang-switch"

if [ ! -d /opt/devkitpro ]; then
    # On the host: the source (the container may have no network), then
    # the build in the NVK image.
    if [ ! -d "$SRC/.git" ]; then
        mkdir -p "$(dirname "$SRC")"
        git clone --quiet --depth 1 --branch "$GLSLANG_REF" "$GLSLANG_REPO" "$SRC"
    fi
    echo "glslang $GLSLANG_REF: $(git -C "$SRC" rev-parse --short HEAD)"
    VULKAN=1 exec bash "$MC3_REPO/switch/docker_build.sh" switch/build_glslang.sh
fi

# In the container.
BUILD="$MC3_REPO/work-dub/deps/build-glslang-switch"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$OUT" \
    -DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=OFF -DENABLE_HLSL=OFF \
    -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF -DBUILD_EXTERNAL=OFF \
    -DENABLE_SPVREMAPPER=OFF >/dev/null
ninja -C "$BUILD" install >/dev/null
echo "glslang for the Switch in ${OUT#$MC3_REPO/}"
ls "$OUT/lib"
