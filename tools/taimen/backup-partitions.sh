#!/usr/bin/env bash
# Dump the Pixel 2 XL (taimen) partitions the iOS bring-up needs, one at a
# time over a rooted ADB shell, and verify each copy against a checksum
# computed on the phone. Prints one line per partition as it lands.
#
# Usage: tools/backup-partitions.sh [OUTDIR]   (default: backup/)
set -u
OUT=${1:-backup}
mkdir -p "$OUT"
cd "$OUT" || exit 1

# Required by the ios-sweet / ios-flame / ios-sailfish READMEs:
REQUIRED="persist modemst1 modemst2 fsg fsc boot_a boot_b dtbo_a dtbo_b vbmeta_a vbmeta_b"
# Extra, tiny, belt-and-braces (bootloader chain and device info):
EXTRA="abl_a abl_b xbl_a xbl_b devinfo ssd sec misc frp cdt ddr"
PARTS="$REQUIRED $EXTRA"

if ! timeout 10 adb shell id | grep -q 'uid=0'; then
  echo "need a root adb shell (adb root); aborting" >&2
  exit 1
fi

: > sha256-device.txt
fail=0
for p in $PARTS; do
  dev=$(timeout 10 adb shell "readlink -f /dev/block/by-name/$p" | tr -d '\r')
  if [ -z "$dev" ]; then printf "%-9s  MISSING on device\n" "$p"; fail=1; continue; fi
  sz=$(timeout 10 adb shell "blockdev --getsize64 $dev" | tr -d '\r')
  timeout 600 adb exec-out "dd if=$dev bs=1M 2>/dev/null" > "$p.img"
  got=$(stat -c %s "$p.img")
  dsum=$(timeout 300 adb shell "sha256sum $dev" | awk '{print $1}')
  hsum=$(sha256sum "$p.img" | awk '{print $1}')
  if [ "$sz" = "$got" ] && [ "$dsum" = "$hsum" ]; then st=OK; else st=MISMATCH; fail=1; fi
  printf "%-9s %-16s %10s bytes  %s\n" "$p" "$dev" "$got" "$st"
  echo "$dsum  $p.img" >> sha256-device.txt
done

echo
du -sh . | awk '{print "total: " $1}'
if [ $fail -eq 0 ]; then echo "ALL $(wc -l < sha256-device.txt) PARTITIONS VERIFIED"; else echo "SOME PARTITIONS FAILED, see above" >&2; exit 1; fi
