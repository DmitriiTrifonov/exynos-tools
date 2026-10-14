#!/usr/bin/env bash
# Phase 4 check: run vk_probe with the layer embedded and EXYNOS_TOOLS_BC=<mode>, then assert the
# reporting is consistent. usage: scripts/check-advertise.sh [build-dir] [mode]
#   mode advertise-force (default): pretends the driver has no BC4-BC7 (works on any driver)
#   mode advertise: only emulates what the driver really lacks (use this on the Flip 7)
# Expectations (phase 4): textureCompressionBC=1 in features and features2; every BC format has
# sampled+transfer-dst features and a successful image-format query. (Phase 4 made image creation
# fail in advertise mode; since phase 5 the layer no longer touches creation in advertise modes, the
# driver decides. Real emulation is covered by bc_upload_test with EXYNOS_TOOLS_BC=emulate.)
set -u
B="${1:-build}"; MODE="${2:-advertise-force}"
LIB="$(cd "$B" && pwd)/libVkLayer_exynos_tools.so"
OUT="$B/probe-$MODE.txt"
EXYNOS_TOOLS_BC="$MODE" EXYNOS_PROBE_LAYER="$LIB" "$B/vk_probe" > "$OUT" 2> "$B/probe-$MODE.err" || { echo "FAIL: probe exited non-zero"; exit 1; }
fail=0
chk() { if [ "$2" = "ok" ]; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

grep -q "textureCompressionBC=1" "$OUT" && chk "features.textureCompressionBC=1" ok || chk "features.textureCompressionBC=1" no
grep -q "features2 textureCompressionBC=1" "$OUT" && chk "features2.textureCompressionBC=1" ok || chk "features2.textureCompressionBC=1" no

# format table: lines "    BCx_... 0xOPT 0xLIN 0xBUF" between "  formats" and "  bc image-format"
bad=""
while read -r name opt _; do
  case "$name" in BC*) (( (opt & 0x8001) == 0x8001 )) || bad="$bad $name";; esac
done < <(sed -n '/^  formats/,/^  bc image-format/p' "$OUT")
[ -z "$bad" ] && chk "all BC formats: sampled + transfer-dst" ok || chk "all BC formats: sampled + transfer-dst ($bad)" no

q=$(awk '/^  bc image-format/ {f=1; next} /^  create 64x64/ {f=0} f && $2 != "query=0" {print $1}' "$OUT")
[ -z "$q" ] && chk "all BC image-format queries succeed" ok || chk "all BC image-format queries succeed ($q)" no
q2=$(awk '/^  bc image-format/ {f=1; next} /^  create 64x64/ {f=0} f && ($3 != "query2=0" || $5 != "fp2same=1") {print $1}' "$OUT")
[ -z "$q2" ] && chk "FormatProperties2/ImageFormatProperties2 agree" ok || chk "FormatProperties2/ImageFormatProperties2 agree ($q2)" no

exit $fail
