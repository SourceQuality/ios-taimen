#!/usr/bin/env bash
# One kernel boot attempt on the Pixel 2 XL, end to end:
#   kernel Mach-O -> ADT (mkadt --taimen) -> SWEETXNU payload (mkpayload, 2 MB
#   L2 blocks) -> STAGE2 shim boot image with the payload as ramdisk ->
#   fastboot boot -> wait for the phone to come back (watchdog or PSCI reset
#   lands in Android) -> pull the shim log from UFS.
# Usage: tools/taimen/boot-xnu.sh KERNEL [OUTDIR] [extra boot-args]
set -euo pipefail
P=$(cd "$(dirname "$0")/../.." && pwd)
kernel=$(realpath "$1"); out=${2:-$P/out/taimen-xnu}; shift || true; shift || true
args=${*:-}
mkdir -p "$out"
export PATH="$HOME/.local/bin:$PATH"
python3 -I "$P/tools/mkadt.py" "$out/taimen.adt" --taimen
# cpus=1 and no root yet: the first milestone is XNU painting its own bands.
cmdline="debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress ${args}"
python3 -I "$P/tools/mkpayload.py" "$kernel" "$out/taimen.adt" "$out/payload" --l2block 0x200000 --cmdline "$cmdline"
ls -l "$out/payload" | awk '{print "payload: " $5 " bytes"}'
EXTRA_CFLAGS=-DSTAGE2 OUT_NAME=taimen-xnu "$P/shim/build-taimen.sh" "$P/../backup/boot_a.img" "$out/payload" | tail -2
img="$P/shim/out/taimen-xnu.img"
# Get to fastboot from wherever the phone is.
state=$(timeout 5 adb get-state 2>/dev/null || true)
if [ "$state" = "device" ] || [ "$state" = "recovery" ]; then timeout 15 adb reboot bootloader || true; fi
for i in $(seq 1 60); do fastboot devices 2>/dev/null | grep -q . && break; sleep 1; done
fastboot devices | grep -q . || { echo "phone not in fastboot" >&2; exit 1; }
t0=$(date +%s)
echo "fastboot boot at $(date +%T): $img"
fastboot boot "$img" | tail -1
back=""
for i in $(seq 1 240); do
	s=$(timeout 3 adb get-state 2>/dev/null || true)
	if [ "$s" = "device" ]; then back="android"; break; fi
	if [ $i -gt 20 ] && fastboot devices 2>/dev/null | grep -q .; then back="fastboot"; break; fi
	sleep 1
done
echo "+$(( $(date +%s)-t0 ))s: back in ${back:-nothing (hung? power key needed)}"
if [ "$back" = "android" ]; then
	echo "bootreason: $(timeout 5 adb shell getprop ro.boot.bootreason 2>/dev/null)"
	"$P/tools/taimen/log-pull.sh" "$out/shim-log.txt" | sed -n '2,200p'
fi
