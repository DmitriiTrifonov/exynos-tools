#!/usr/bin/env bash
# Run vk_probe plain, through the layer via the loader, and through the layer in embedded mode,
# and diff what the application sees. usage: scripts/run-probe.sh [build-dir]  (default: build)
# Loader mode only works with loaders that honour VK_INSTANCE_LAYERS (not Android's system loader).
set -u
B="${1:-build}"
LIB="$(cd "$B" && pwd)/libVkLayer_exynos_tools.so"
export VK_LAYER_PATH="$B"
"$B/vk_probe" > "$B/probe-plain.txt" 2> "$B/probe-plain.err"; echo "plain exit: $?"
VK_INSTANCE_LAYERS=VK_LAYER_EXYNOS_TOOLS_xclipse "$B/vk_probe" > "$B/probe-loader.txt" 2> "$B/probe-loader.err"
echo "loader-mode exit: $?"
EXYNOS_PROBE_LAYER="$LIB" "$B/vk_probe" > "$B/probe-embedded.txt" 2> "$B/probe-embedded.err"
echo "embedded exit: $?"
echo "--- embedded-mode log (stderr), first lines ---"; head -12 "$B/probe-embedded.err"
echo "--- loader-mode log lines: $(grep -c exynos-tools "$B/probe-loader.err") ; embedded-mode log lines: $(grep -c exynos-tools "$B/probe-embedded.err")"
rc=0
for m in loader embedded; do
  if diff -q "$B/probe-plain.txt" "$B/probe-$m.txt" >/dev/null; then echo "$m: IDENTICAL to plain"; else echo "$m: DIFFERS"; rc=1; fi
done
exit $rc
