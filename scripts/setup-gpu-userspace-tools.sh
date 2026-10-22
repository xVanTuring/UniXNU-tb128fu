#!/opt/homebrew/bin/bash
# Tools the GPU user-space build needs on this Mac (UniXNU scripts/build-turnip-darwin.py, build-native-metal-bridge.py,
# build-adreno-metal-plugin.py), for the TB128FU tree: glslangValidator, a current bison, glslc (shaderc: the plugin's smoke
# shaders, gen-turnip-smoke-shaders.py) and the Vulkan headers (check-turnip-darwin-host.py's host smoke) from Homebrew, a Python venv with
# meson and Mesa's generator modules at <tree>/.cache/mesa-venv, bison linked into <tree>/.cache/mesa-tools/bin (both put on
# PATH by the build scripts), and Rust 1.98.0 with the aarch64-apple-ios target (metal2vulkan's pinned toolchain).
# Run it yourself once: ./scripts/setup-gpu-userspace-tools.sh
# MIRROR=tuna takes Homebrew bottles and its API and PyPI from mirrors.tuna.tsinghua.edu.cn (not rustup: that mirror has no
# pinned point releases such as 1.98.0, a 404); every MIRROR value also skips Homebrew's auto-update before the install. Steps
# already done are skipped or cheap on a rerun.
set -euo pipefail
# The build tree of the UniXNU checkout next to this repository, unless TB128FU_TREE names another.
TREE=${TB128FU_TREE:-$(cd "$(dirname "$0")/../.." && pwd)/UniXNU/out/tb128fu/tree}
[ -d "$TREE" ] || { echo "no build tree at $TREE (run a unixnu build first)"; exit 1; }
if [ -n "${MIRROR:-}" ]; then
  export HOMEBREW_NO_AUTO_UPDATE=1
fi
if [ "${MIRROR:-}" = tuna ]; then
  export HOMEBREW_API_DOMAIN=https://mirrors.tuna.tsinghua.edu.cn/homebrew-bottles/api
  export HOMEBREW_BOTTLE_DOMAIN=https://mirrors.tuna.tsinghua.edu.cn/homebrew-bottles
  export UV_DEFAULT_INDEX=https://pypi.tuna.tsinghua.edu.cn/simple
fi

brew install glslang bison shaderc vulkan-headers

uv venv --allow-existing --python 3.13 "$TREE/.cache/mesa-venv"
uv pip install --python "$TREE/.cache/mesa-venv/bin/python" meson mako pyyaml packaging pycparser

mkdir -p "$TREE/.cache/mesa-tools/bin"
ln -sf "$(brew --prefix bison)/bin/bison" "$TREE/.cache/mesa-tools/bin/bison"

# A toolchain whose installation was interrupted is listed but has no manifest: remove it and install it again.
if ! rustc +1.98.0 --version >/dev/null 2>&1; then
  rustup toolchain uninstall 1.98.0 >/dev/null 2>&1 || true
  rustup toolchain install 1.98.0 --profile minimal
fi
rustup target add aarch64-apple-ios --toolchain 1.98.0

echo "glslangValidator: $(command -v glslangValidator)"
echo "glslc: $(command -v glslc)"
echo "vulkan headers: $(ls "$(brew --prefix vulkan-headers)/include/vulkan/vulkan.h")"
echo "meson: $("$TREE/.cache/mesa-venv/bin/meson" --version)"
echo "bison: $("$TREE/.cache/mesa-tools/bin/bison" --version | head -1)"
echo "rust: $(rustc +1.98.0 --version)"
