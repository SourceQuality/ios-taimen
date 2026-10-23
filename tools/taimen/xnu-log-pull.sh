#!/usr/bin/env bash
# Pull the PREVIOUS boot's ramoops console zone from Android: Google's
# access_ramoops driver exposes the alternate ramoops region (0xb0e00000,
# 2 MiB) as /dev/access-ramoops, and Linux copies the main region there at
# every boot before reusing it. XNU's early log (sweet_early.c, TAIMEN.h:
# SWEET_EARLY_LOG_PRZ 0xb0080000) therefore shows up here right after the
# watchdog reset that follows a kernel hang or panic. Layout of the 2 MiB
# region: 4 x 128 KiB dump zones, then the 1 MiB console zone at +0x80000
# (persistent_ram_buffer: sig "DBGC", start, size, text), then 512 KiB pmsg.
# Usage: tools/taimen/xnu-log-pull.sh [OUT.txt]   (rooted ADB in Android)
set -euo pipefail
out=${1:-xnu-ramoops.txt}
adb root >/dev/null 2>&1 || true
sleep 2
adb wait-for-device
adb exec-out "dd if=/dev/access-ramoops bs=4096 skip=128 count=256 2>/dev/null" > "$out.bin"
python3 -I - "$out.bin" "$out" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read()
sig, start, size = struct.unpack_from('<3I', b, 0)
if sig != 0x43474244:
    print(f'no persistent_ram record in the alt console zone (sig {sig:#x})'); sys.exit(1)
data = b[12:12 + min(size, len(b) - 12)]
# ring: data[start..size) + data[0..start) when it has wrapped
text = (data[start:] + data[:start]) if start < size else data
text = text.replace(b'\r', b'')
open(sys.argv[2], 'wb').write(text)
print(f'{sys.argv[2]}: {len(text)} bytes (record size {size}, start {start})')
EOF
rm -f "$out.bin"
