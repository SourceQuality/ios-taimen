#!/usr/bin/env bash
# Pull XNU's early log after a boot attempt, from Android (rooted ADB).
#
# XNU (sweet_early.c, TAIMEN.h: SWEET_EARLY_LOG_PRZ 0xb0080000) appends to the
# ramoops console zone. RAM survives the watchdog reset, and the Android boot
# that follows exposes that zone, with Linux's own console appended after it,
# as /sys/fs/pstore/console-ramoops (seen 2026-10-22; the earlier boot showed
# it only in the alt region, which the vendor ramoops_sh service then encrypts,
# so /dev/access-ramoops is only a fallback here).
# Usage: tools/taimen/xnu-log-pull.sh [OUT.txt]
set -euo pipefail
out=${1:-xnu-log.txt}
adb root >/dev/null 2>&1 || true
sleep 2
adb wait-for-device
# TWRP (the boot slot during the root phase) has pstore but does not mount it
adb shell 'mountpoint -q /sys/fs/pstore 2>/dev/null || mount -t pstore pstore /sys/fs/pstore 2>/dev/null; true' >/dev/null 2>&1 || true
raw="$out.raw"
if adb shell 'test -r /sys/fs/pstore/console-ramoops' 2>/dev/null; then
	adb exec-out 'cat /sys/fs/pstore/console-ramoops' > "$raw"
	src=/sys/fs/pstore/console-ramoops
else
	adb exec-out "dd if=/dev/access-ramoops bs=4096 skip=128 count=256 2>/dev/null" > "$raw" || true
	src=/dev/access-ramoops
fi
python3 -I - "$raw" "$out" "$src" <<'EOF'
import re, struct, sys
b = open(sys.argv[1], 'rb').read()
if sys.argv[3] != '/sys/fs/pstore/console-ramoops':
    sig, start, size = struct.unpack_from('<3I', b, 0)
    if bin((sig ^ 0x43474244) & 0xffffffff).count('1') > 2 or not (0 < size <= 0x100000 - 12):
        print(f'no usable record in {sys.argv[3]} (sig {sig:#x}, size {size})'); sys.exit(1)
    data = b[12:12 + min(size, len(b) - 12)]
    b = (data[start:] + data[:start]) if start < size else data
b = b.replace(b'\r', b'')
# XNU's part: from the shim/kernel markers up to Linux's "Booting Linux" line that follows it
m = re.search(rb'(TAIMEN SHIM was here\n)?SWEET EARLY 0: start\.s entered', b)
if not m:
    print(f'no XNU log in {sys.argv[3]} ({len(b)} bytes)'); sys.exit(1)
tail = b[m.start():]
e = tail.find(b'Booting Linux on physical CPU')
if e > 0:
    tail = tail[:e]
open(sys.argv[2], 'wb').write(tail)
print(f'{sys.argv[2]}: {len(tail)} bytes of XNU log from {sys.argv[3]}')
EOF
rm -f "$raw"
