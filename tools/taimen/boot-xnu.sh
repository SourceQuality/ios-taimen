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
# ROOT=1: the iOS root on userdata (tools/taimen/install-root.sh). The args are the Pixel 1
# README's line minus its GPU/Wi-Fi parts plus what docs/taimen-audit-root-path.md asks for:
# ufsrw=1 (HFS+ journal replay, UFS console ring), cs_enforcement_disable=1 (4K pages vs the
# 16K-signed cache), the userland switches dyld/launchd/watchdogd read from kern.bootargs;
# sweetfb_noconsole=1: no kernel text over the GUI (the log reader brings the console back).
# cpus=8 taimengold=1728: all cores, the gold cluster moved from GPLL0 / 2 (299 MHz, where ABL
# leaves it; boots 39-41) onto its own PLL at 1728 MHz at the boot voltage (xnu 0226, boot 46:
# every step self-tested, gold memcpy 7.9 GB/s); sweetkeys=1: power, volume up, volume down as
# Home (xnu 0225-0226); taimenhwscale=3: QSEED3 full screen from two scan buffers (xnu 0218, boot
# 42; boot 91: present 8-15 ms against 27-31 ms with one buffer, every swap shown); sweetperf=1 taimenddr=1804: the perf thread votes DDR to 1804 MHz through
# the RPM (xnu 0224, boot 44: bimc 648 -> 902 MHz, memcpy ~0.8 -> 1.09 GB/s); taimenamp=1: idle
# placement gold first above utility priority, silver for utility and background (xnu 0248, boots
# 82-83: Control Center frames under 20 ms 41 -> 69 %); taimenosm=33: gold through CPRh and the
# OSM as Linux runs it (xnu 0252, boot 98: 2342 MHz at 1052 mV, ~45 frames/s in the swipes against
# ~17 at 1728), G1's taimengold=1728 if it refuses; taimenosmsilver=21: silver the same way to
# 1900.8 MHz at 928 mV (xnu 0256, boot 100). Later extra args override all of these
# (the last occurrence wins).
if [ "${ROOT:-0}" = 1 ]; then
	args="cpus=8 taimenosm=33 taimenosmsilver=21 taimengold=1728 taimenamp=1 sweetkeys=1 taimenhwscale=3 sweetperf=1 taimenddr=1804 rd=ufsroot0 ufsrw=1 cs_enforcement_disable=1 unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1 wdt_test=no_user_checkin sweetfb_noconsole=1 ${args}"
fi
# GPU (ROOT=1 boots once tools/taimen/sweetgl-install.sh has run; GPU=0: CoreAnimation's CPU
# renderer): CA draws with Mesa on the A540, sweetgl injected into backboardd (boots 60-75: ~60
# frames/s in the swipe animations). 2.96 GiB of RAM from 0xb1000000 (xnu 0250-0251, 0254, boot 96:
# the low bank's top runs on into the upper bank); the GPU service at 596 MHz (sweetgpu=8, the CP
# microcode and zap shader from GPUFW); the A540 copies each frame to the scan buffer
# (taimengpucopy, E5); Mesa's buffers write-combined from a 256 MB heap above XNU's RAM (xnu 0240,
# XNU's RAM ends at 0x16e400000). The dylib comes with the sweetgl ROOTFIX, whatever ROOTFIX says:
# without the file backboardd cannot start (DYLD_INSERT_LIBRARIES).
sweetgl_env=$P/../out/sweetgl/current.env
if [ "${ROOT:-0}" = 1 ] && [ "${GPU:-1}" = 1 ] && [ -f "$sweetgl_env" ]; then
	. "$sweetgl_env"
	TAIMEN_BASE=${TAIMEN_BASE:-0xb1000000}
	TAIMEN_MEM=${TAIMEN_MEM:-0xbd400000}
	GPUFW=${GPUFW:-$P/../research/taimen/firmware}
	if [ -n "${ROOTFIX:-}" ] && [ "$(realpath "$ROOTFIX")" != "$(realpath "$SWEETGL_ROOTFIX")" ]; then
		echo "GPU boot: ROOTFIX $SWEETGL_ROOTFIX (installs $SWEETGL_LIB) instead of $ROOTFIX" >&2
	fi
	ROOTFIX=$SWEETGL_ROOTFIX
	args="sweetgpu=8 sweetgpuclk=600 taimengpucopy=1 taimengpuheap=256 sweetenv=backboardd:DYLD_INSERT_LIBRARIES=$SWEETGL_LIB sweetnull=backboardd ${args}"
	echo "GPU boot: sweetgl ${SWEETGL_SHA:0:7} ($SWEETGL_LIB), RAM ${TAIMEN_BASE:-0x100000000} + $TAIMEN_MEM"
fi
# SHIM_WATCH=1: CPU 7 stays in the shim as a watcher (shim/taimen.c WATCH): XNU gets 7 cores, and
# if its early log stops growing before its UFS driver starts, the watcher saves the log to the
# shim log on UFS and pulls PS_HOLD (early hangs otherwise end in a watchdog reset with no log).
shim_cflags=-DSTAGE2
if [ "${SHIM_WATCH:-0}" = 1 ]; then
	shim_cflags="-DSTAGE2 -DWATCH"
	args="${args} cpus=7"
fi
mkdir -p "$out"
export PATH="$HOME/.local/bin:$PATH"
# TAIMEN_BASE=0xb1000000 / TAIMEN_MEM=BYTES: XNU's RAM (mkadt.py; default 512 MiB at 0x100000000)
TAIMEN_NCPU=$([ "${SHIM_WATCH:-0}" = 1 ] && echo 7) TAIMEN_BASE=${TAIMEN_BASE:-} TAIMEN_MEM=${TAIMEN_MEM:-} python3 -I "$P/tools/mkadt.py" "$out/taimen.adt" --taimen
# cpus=1 and no root yet: the first milestone is XNU painting its own bands.
# debug 0x10000c: 0xc plus DB_REBOOT_ALWAYS; DB_NMI alone sets panicDebugging, which parks a
# panicked kernel instead of resetting (no watchdog bite since boot 13), so a panic now ends in
# PS_HOLD and the log reader.
# The Pixel 1 keys/touch/usb/perf/battery/backlight drivers are compiled out on TAIMEN
# (xnu 0187) and their boot-args are set to 0 as well (docs/taimen-boot-audit.md).
safe="sweetkeys=0 sweettouch=0 sweetusb=0 sweetperf=0 sweetbattery=0 sweetcharge=0 sweetbl=0"
# taimenepoch: the PC clock for the calendar (xnu 0217; no RTC driver yet)
cmdline="debug=0x10000c serial=3 cpus=1 keepsyms=1 -noprogress taimenepoch=$(date +%s) ${safe} ${args}"
# XNU's PE_parse_boot_argn takes the last occurrence: drop the earlier ones (cpus=1, sweetkeys=0, ...)
cmdline=$(printf '%s\n' "$cmdline" | awk '{ for (i = NF; i >= 1; i--) { k = $i; sub(/=.*/, "", k);
	if (!(k in seen)) { seen[k] = 1; keep[++n] = $i } }
	for (i = n; i >= 1; i--) printf "%s%s", keep[i], (i > 1 ? " " : "\n") }')
# mkpayload.py keeps 607 bytes (BOOT_LINE_LENGTH 608) and cuts the rest silently; the GPU boots
# (sweetgpu, sweetenv for sweetgl) come close, so refuse instead.
[ ${#cmdline} -le 607 ] || { echo "boot-args are ${#cmdline} bytes, over the 607 mkpayload keeps" >&2; exit 1; }
# ROOTFIX=FILE: files the kernel writes into the iOS root before launchd (tools/taimen/mkrootfix.py, xnu 0210)
blobs=()
[ -n "${ROOTFIX:-}" ] && blobs+=(--blob "ROOTFIX=$(realpath "$ROOTFIX")")
# GPUFW=DIR: the Adreno CP microcode for sweetgpu>=2 (docs/taimen-gpu-plan.md 6): the vendor
# a530_pm4.fw / a530_pfp.fw (pulled by the log reader, never in git) without their 4-byte header.
if [ -n "${GPUFW:-}" ]; then
	for f in pm4 pfp; do tail -c +5 "$GPUFW/a530_$f.fw" > "$out/gpu-$f.bin"; done
	blobs+=(--blob "GPU-PM4=$out/gpu-pm4.bin" --blob "GPU-PFP=$out/gpu-pfp.bin")
	# the zap shader for sweetgpu>=3: the .mdt (headers + hash) and its one loadable segment
	[ -f "$GPUFW/a540_zap.mdt" ] && blobs+=(--blob "GPU-ZAP-MDT=$GPUFW/a540_zap.mdt" --blob "GPU-ZAP-SEG=$GPUFW/a540_zap.b02")
fi
python3 -I "$P/tools/mkpayload.py" "$kernel" "$out/taimen.adt" "$out/payload" --l2block 0x200000 --cmdline "$cmdline" "${blobs[@]}"
# ABL does not start a boot image whose ramdisk (the payload) is over ~35 MiB: boot 77's 36698688
# bytes booted, boot 78's 36715104 went straight back to fastboot (shim never ran). The sweetgl
# ROOTFIX carries the 19 MB dylib; once it is on the phone (write-if-absent) the base ROOTFIX
# does as well, and xnu 0244 leaves a missing DYLD_INSERT_LIBRARIES file out (CPU renderer).
abl_max=$((35 << 20))
base_fix=$P/../research/taimen/rootfix.bin
if [ "$(stat -c %s "$out/payload")" -gt $abl_max ] && [ -n "${SWEETGL_ROOTFIX:-}" ] && [ -f "$base_fix" ] &&
	[ "$(realpath "${ROOTFIX:-/}")" = "$(realpath "$SWEETGL_ROOTFIX")" ]; then
	echo "payload $(stat -c %s "$out/payload") bytes, over the $abl_max ABL starts: $base_fix instead (the dylib from an earlier boot)"
	blobs=("${blobs[@]/#ROOTFIX=*/ROOTFIX=$(realpath "$base_fix")}")
	python3 -I "$P/tools/mkpayload.py" "$kernel" "$out/taimen.adt" "$out/payload" --l2block 0x200000 --cmdline "$cmdline" "${blobs[@]}"
fi
[ "$(stat -c %s "$out/payload")" -le $abl_max ] || { echo "payload over the $abl_max bytes ABL starts" >&2; exit 1; }
ls -l "$out/payload" | awk '{print "payload: " $5 " bytes"}'
EXTRA_CFLAGS=$shim_cflags OUT_NAME=taimen-xnu "$P/shim/build-taimen.sh" "$P/../backup/boot_a.img" "$out/payload" | tail -2
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
for i in $(seq 1 "${BOOT_WAIT:-240}"); do
	s=$(timeout 3 adb get-state 2>/dev/null || true)
	if [ "$s" = "device" ]; then back="android"; break; fi
	if [ "$s" = "recovery" ]; then back="twrp"; break; fi
	if [ $i -gt 20 ] && fastboot devices 2>/dev/null | grep -q .; then back="fastboot"; break; fi
	sleep 1
done
echo "+$(( $(date +%s)-t0 ))s: back in ${back:-nothing (hung? power key needed)}"
[ -n "$back" ] || [ -n "${NO_ALERT:-}" ] || "$(cd "$(dirname "$0")" && pwd)/attention.sh" "Pixel 2 XL did not come back after the boot: check the screen, then hold power 10 seconds"
if [ "$back" = "android" ] || [ "$back" = "twrp" ]; then
	echo "bootreason: $(timeout 5 adb shell getprop ro.boot.bootreason 2>/dev/null)"
	"$P/tools/taimen/log-pull.sh" "$out/shim-log.txt" | grep -E 'LOADER|saved|EXC|wdt' || true
	echo "=== XNU log (previous boot's ramoops console zone, via /dev/access-ramoops):"
	"$P/tools/taimen/xnu-log-pull.sh" "$out/xnu-log.txt" && cat "$out/xnu-log.txt"
fi
