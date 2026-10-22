#!/opt/homebrew/bin/bash
# Downloads the Rust crates of the native Metal bridge (UniXNU scripts/build-native-metal-bridge.py: the pinned metal2vulkan runtime
# behind a C ABI) for the TB128FU tree, so the later builds run offline. Run it yourself once:
#   ./scripts/fetch-gpu-userspace-crates.sh
# MIRROR=rsproxy takes crates.io from rsproxy.cn, MIRROR=ustc from mirrors.ustc.edu.cn; without MIRROR, crates.io itself.
# Needs the metal2vulkan checkout at <tree>/.cache/gpu-reference/metal2vulkan and Rust 1.98.0 (setup-gpu-userspace-tools.sh).
set -euo pipefail
# The build tree of the UniXNU checkout next to this repository, unless TB128FU_TREE names another.
TREE=${TB128FU_TREE:-$(cd "$(dirname "$0")/../.." && pwd)/UniXNU/out/tb128fu/tree}
[ -d "$TREE/.cache/gpu-reference/metal2vulkan/.git" ] || { echo "no metal2vulkan checkout under $TREE/.cache/gpu-reference"; exit 1; }

# The bridge's Cargo.toml, written exactly as the build writes it (patched translator source, fixtures), without running cargo.
manifest=$(python3 "$TREE/scripts/build-native-metal-bridge.py" --manifest-only | tail -1)
[ -f "$manifest" ] || { echo "manifest not written"; exit 1; }

config=()
case "${MIRROR:-}" in
  rsproxy) config=(--config 'source.crates-io.replace-with="mirror"' --config 'source.mirror.registry="sparse+https://rsproxy.cn/index/"') ;;
  ustc) config=(--config 'source.crates-io.replace-with="mirror"' --config 'source.mirror.registry="sparse+https://mirrors.ustc.edu.cn/crates.io-index/"') ;;
  '') ;;
  *) echo "unknown MIRROR=$MIRROR (rsproxy, ustc or empty)"; exit 1 ;;
esac

# Resolves Cargo.lock next to the manifest and downloads every crate for both targets the build uses.
cargo +1.98.0 "${config[@]}" fetch --manifest-path "$manifest" --target aarch64-apple-ios
cargo +1.98.0 "${config[@]}" fetch --manifest-path "$manifest" --target aarch64-apple-darwin
echo "crates: $(grep -c '^name = ' "$(dirname "$manifest")/Cargo.lock") packages in $(dirname "$manifest")/Cargo.lock"
