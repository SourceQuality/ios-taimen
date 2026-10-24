#!/usr/bin/env bash
# One unattended XNU boot on the Pixel 2 XL, from fastboot back to fastboot:
#   reader image (tools/taimen/mkreader.sh) on both boot slots -> tools/taimen/boot-xnu.sh ->
#   XNU runs until taimenreboot, PS_HOLD reset -> ABL boots the reader -> the reader's USB
#   drive is copied to OUTDIR/reader (tools/taimen/reader-pull.ps1) -> the reader reboots
#   into fastboot. Decoded logs: OUTDIR/sweetlog.txt (UFS ring), OUTDIR/pstore-xnu.txt.
# Usage: [ROOT=1] tools/taimen/autoboot.sh KERNEL OUTDIR [boot-args]   (include taimenreboot=N)
# Exit: 0 logs read and phone back in fastboot; 2 no fastboot at the start; 3 no reader drive
# (phone hung: needs the power key); 4 logs read but the phone did not return to fastboot;
# 5 the reader image did not build.
set -uo pipefail
P=$(cd "$(dirname "$0")/../.." && pwd)
export PATH="$HOME/.local/bin:$PATH"
kernel=$(realpath "$1"); out=$(realpath -m "$2"); shift 2
mkdir -p "$out"
say() { echo "$(date +%T) $*"; }
fb() { timeout 5 fastboot devices 2>/dev/null | grep -q .; }
waitfb() { for i in $(seq 1 "$1"); do fb && return 0; sleep 2; done; return 1; }

waitfb 60 || { say "phone not in fastboot"; "$P/tools/taimen/attention.sh" "Put the Pixel in fastboot mode: hold power and volume down"; waitfb 300 || exit 2; }
img=$P/shim/out/taimen-reader.img
"$P/tools/taimen/mkreader.sh" > /dev/null || { say "reader build failed"; exit 5; }
# Flash the reader only when it changed (both slots take ~15 s). The image itself is not
# byte-reproducible (boot image signing), so the hash covers its inputs: the initramfs and
# kernel mkreader.sh produced, and the scripts. FLASH_READER=1 forces a flash.
rh=$(cat "$P/../reader/initramfs.cpio.gz" "$P/../reader/Image" "$P/tools/taimen/mkreader.sh" \
	"$P/tools/taimen/mkboot.sh" | sha256sum | cut -d' ' -f1)
rstamp=$P/../out/reader-flashed.sha256
if [ "${FLASH_READER:-0}" = 1 ] || [ "$(cat "$rstamp" 2>/dev/null)" != "$rh" ]; then
	rm -f "$rstamp"
	flashed=0
	for s in a b; do
		timeout 120 fastboot flash boot_$s "$img" > "$out/flash-$s.log" 2>&1 && flashed=$((flashed + 1))
		grep -E "Writing|FAILED" "$out/flash-$s.log"
	done
	[ "$flashed" = 2 ] && echo "$rh" > "$rstamp"
else
	say "reader unchanged, not reflashed"
fi
reboot_s=$(echo "$*" | sed -n 's/.*taimenreboot=\([0-9]*\).*/\1/p')
NO_ALERT=1 BOOT_WAIT=25 "$P/tools/taimen/boot-xnu.sh" "$kernel" "$out" "$@" > "$out/boot.log" 2>&1
grep -E "warning|fastboot boot at|Booting" "$out/boot.log"
rm -rf "$out/reader"
# kernel start ~25 s after the fastboot boot, reset + ABL + reader + USB ~60 s, more under load
# (boot 30: the drive came ~4 min after the deadline)
if ! powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$P/tools/taimen/reader-pull.ps1")" \
	-Dest "$(wslpath -w "$out/reader")" -Timeout $(( ${reboot_s:-300} + 600 )) | tr -d '\r'; then
	say "no reader drive: the phone hung or the reader did not start"
	"$P/tools/taimen/attention.sh" "Pixel 2 XL hung. Hold power and volume down for 10 seconds"
	exit 3
fi
[ -f "$out/reader/sweetlog.bin" ] && python3 -I "$P/tools/sweet-log.py" "$out/reader/sweetlog.bin" "$out/sweetlog.txt"
[ -f "$out/reader/shim-log.bin" ] && tr -d '\000' < "$out/reader/shim-log.bin" > "$out/shim-log.txt"
python3 -I - "$out/reader" "$out/pstore-xnu.txt" <<'EOF'
import glob, re, sys
best = b''
for f in glob.glob(sys.argv[1] + '/pstore-console-ramoops*'):
    b = open(f, 'rb').read().replace(b'\r', b'')
    m = re.search(rb'(TAIMEN SHIM was here\n)?SWEET EARLY [01]: ', b)
    if m:
        t = b[m.start():]
        e = t.find(b'Booting Linux on physical CPU')
        t = t[:e] if e > 0 else t
        best = max(best, t, key=len)
open(sys.argv[2], 'wb').write(best)
print(f'{sys.argv[2]}: {len(best)} bytes of XNU log from pstore')
EOF
for f in "$out/sweetlog.txt" "$out/pstore-xnu.txt"; do
	[ -s "$f" ] && grep -q '^SHOT ' "$f" && python3 -I "$P/tools/taimen/shots.py" "$f" "$out/shots"
done
say "logs in $out; waiting for the reader's reboot into fastboot"
waitfb 90 && { say "phone back in fastboot"; exit 0; }
say "phone not back in fastboot"
"$P/tools/taimen/attention.sh" "The Pixel did not return to fastboot. Hold power and volume down"
exit 4
