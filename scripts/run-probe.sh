#!/usr/bin/env bash
# Run vk_probe without and with the layer and diff what the app sees.
# usage: scripts/run-probe.sh [build-dir]   (default: build)
set -u
B="${1:-build}"
export VK_LAYER_PATH="$B"
"$B/vk_probe" > "$B/probe-plain.txt" 2> "$B/probe-plain.err"; echo "plain exit: $?"
VK_INSTANCE_LAYERS=VK_LAYER_EXYNOS_TOOLS_xclipse "$B/vk_probe" > "$B/probe-layer.txt" 2> "$B/probe-layer.err"
echo "layer exit: $?"
echo "--- layer log (stderr) ---"; cat "$B/probe-layer.err"
echo "--- diff of app-visible output (empty = identical) ---"
diff "$B/probe-plain.txt" "$B/probe-layer.txt" && echo "IDENTICAL"
