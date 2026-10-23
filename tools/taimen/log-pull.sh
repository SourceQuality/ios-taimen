#!/usr/bin/env bash
# Pull the taimen shim's log from the `align_parti` partition (first 64 x 4 KiB
# blocks) over rooted ADB, in Android (LineageOS "Rooted debugging") or TWRP.
# Read-only. Usage: tools/taimen/log-pull.sh [OUT.txt]
set -euo pipefail
out=${1:-shim-log.txt}
adb root >/dev/null 2>&1 || true
sleep 2
adb wait-for-device
dev=$(adb shell 'readlink -f /dev/block/by-name/align_parti' | tr -d '\r')
[ -n "$dev" ] || { echo "align_parti not found" >&2; exit 1; }
adb exec-out "dd if=$dev bs=4096 count=64 2>/dev/null" > "$out.bin"
tr -d '\000' < "$out.bin" > "$out"
rm -f "$out.bin"
echo "$out: $(wc -c < "$out") bytes"
cat "$out"
