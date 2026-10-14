#!/usr/bin/env bash
# Package the layer as a replacement for Bannerlator's "BCn layer" slot (a .tzst = zstd tar that is
# extracted into the container's root file system). Same layout as the leegao_bcn layer:
#   usr/lib/libbcn_layer.so
#   usr/share/vulkan/implicit_layer.d/libbcn_layer.json   (implicit layer, enabled by ENABLE_BCN_COMPUTE=1)
# usage: scripts/package-bannerlator.sh <libVkLayer_exynos_tools.so (Android arm64)> [out.tzst]
set -eu
LIB="${1:?path to libVkLayer_exynos_tools.so}"
OUT="${2:-leegao_bcn.tzst}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/usr/lib" "$TMP/usr/share/vulkan/implicit_layer.d"
cp "$LIB" "$TMP/usr/lib/libbcn_layer.so"
cp "$HERE/launcher/bannerlator/libbcn_layer.json" "$TMP/usr/share/vulkan/implicit_layer.d/libbcn_layer.json"
printf 'version: exynos-tools %s\nnotes: BCn emulation layer for Xclipse (clean-room), github.com/DmitriiTrifonov/exynos-tools\n' \
  "$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo dev)" > "$TMP/version.txt"
OUT_ABS="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
(cd "$TMP" && tar --numeric-owner --owner=0 --group=0 -cf - usr version.txt | zstd -19 -q -o "$OUT_ABS" -f)
echo "wrote $OUT_ABS"
tar --zstd -tvf "$OUT_ABS"
