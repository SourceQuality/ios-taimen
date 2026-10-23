#!/usr/bin/env bash
# Put the iOS root on the Pixel 2 XL and make the phone safe to reset while it is there.
#
#   1. boot_a <- TWRP            (a watchdog reset must never boot Android again: Android
#                                 would find userdata unreadable and reformat it; TWRP just
#                                 fails to mount /data and sits there with ADB)
#   2. userdata <- ios16-root    (the HFS+ root built by tools/mkiosroot.sh, sparse image)
#
# Both steps are reversible from ../backup: `fastboot flash boot_a ../backup/boot_a.img`
# restores LineageOS (its data is gone, it will set up fresh).
# Usage: tools/taimen/install-root.sh ROOT.simg TWRP.img
set -euo pipefail
simg=$(realpath "$1"); twrp=$(realpath "$2")
P=$(cd "$(dirname "$0")/../.." && pwd)
[ -s "$P/../backup/boot_a.img" ] || { echo "no ../backup/boot_a.img: back up first" >&2; exit 1; }
state=$(timeout 5 adb get-state 2>/dev/null || true)
if [ "$state" = "device" ] || [ "$state" = "recovery" ]; then timeout 15 adb reboot bootloader || true; fi
for i in $(seq 1 60); do fastboot devices 2>/dev/null | grep -q . && break; sleep 1; done
fastboot devices | grep -q . || { echo "phone not in fastboot" >&2; exit 1; }
echo "== bootloader: $(fastboot getvar unlocked 2>&1 | head -1), slot $(fastboot getvar current-slot 2>&1 | head -1)"
echo "== 1/2 boot_a <- $(basename "$twrp")"
fastboot flash boot_a "$twrp"
echo "== 2/2 userdata <- $(basename "$simg") ($(stat -c %s "$simg") bytes sparse)"
fastboot flash userdata "$simg"
echo "== done. Resets now land in TWRP. Boot XNU with: tools/taimen/boot-xnu.sh KERNEL OUT \"rd=ufsroot0 ...\""
