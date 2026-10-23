#!/usr/bin/env bash
# Build the taimen log reader boot image (tools/taimen/reader/init): TWRP 3.7.0's kernel
# and DTB (USB mass storage gadget, loop, vfat, pstore/ramoops built in) with a busybox
# initramfs instead of TWRP's ramdisk. The kernel's "skip_initramfs" parameter is renamed
# so the initramfs is used whatever ABL appends to the command line on a normal boot.
# Usage: tools/taimen/mkreader.sh   -> shim/out/taimen-reader.img (flash to boot_a and boot_b)
set -euo pipefail
P=$(cd "$(dirname "$0")/../.." && pwd)
twrp=$P/../research/taimen/twrp/twrp-3.7.0_9-0-taimen.img
w=$P/../reader
mkdir -p "$w" "$P/shim/out"
# static busybox from Ubuntu's arm64 archive (not in git)
v=1.37.0-7ubuntu1
deb=$w/busybox-static_${v}_arm64.deb
[ -s "$deb" ] || curl -fsSLo "$deb" "http://ports.ubuntu.com/ubuntu-ports/pool/main/b/busybox/busybox-static_${v}_arm64.deb"
rm -rf "$w/bb" "$w/root" && dpkg-deb -x "$deb" "$w/bb"
mkdir -p "$w/root/bin"
cp "$w/bb/usr/bin/busybox" "$w/root/bin/busybox"
aarch64-linux-gnu-gcc -static -Os -s -o "$w/root/bin/rebootbl" "$P/tools/taimen/reader/rebootbl.c"
cp "$P/tools/taimen/reader/init" "$w/root/init"
chmod 755 "$w/root/init" "$w/root/bin/busybox" "$w/root/bin/rebootbl"
# newc cpio (no cpio tool needed), with /dev/console for the kernel's first open
python3 -I - "$w/root" "$w/initramfs.cpio.gz" <<'EOF'
import gzip, os, sys
root, out = sys.argv[1:]
ents = [('dev', 0o40755, b''), ('dev/console', 0o20600, b''), ('proc', 0o40755, b''),
        ('sys', 0o40755, b''), ('tmp', 0o40755, b''), ('mnt', 0o40755, b''), ('out', 0o40755, b'')]
for d, _, fs in os.walk(root):
    for n in sorted(fs) + []:
        p = os.path.join(d, n); r = os.path.relpath(p, root)
        ents.append((r, 0o100755, open(p, 'rb').read()))
    rd = os.path.relpath(d, root)
    if rd != '.':
        ents.append((rd, 0o40755, b''))
ents.sort(key=lambda e: e[0].count('/'))
def hdr(ino, name, mode, data, rdev=(0, 0)):
    nm = name.encode() + b'\0'
    h = b'070701' + b''.join(b'%08x' % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, rdev[0], rdev[1], len(nm), 0))
    b = h + nm
    b += b'\0' * (-len(b) % 4) + data
    return b + b'\0' * (-len(b) % 4)
blob = b''.join(hdr(i + 1, n, m, d, (5, 1) if n == 'dev/console' else (0, 0)) for i, (n, m, d) in enumerate(ents))
blob += hdr(0, 'TRAILER!!!', 0, b'')
open(out, 'wb').write(gzip.compress(blob, 9, mtime=0))
print(f'{out}: {len(ents)} entries, {len(blob)} bytes cpio')
EOF
# TWRP's kernel, decompressed, skip_initramfs renamed
python3 -I - "$P/../research/taimen/twrp/kernel" "$w/Image" <<'EOF'
import sys, zlib
k = zlib.decompressobj(31).decompress(open(sys.argv[1], 'rb').read())   # one gzip member, the DTBs follow it
assert k.count(b'skip_initramfs\0') == 1, 'expected one skip_initramfs string'
open(sys.argv[2], 'wb').write(k.replace(b'skip_initramfs\0', b'skip_initramfz\0'))
EOF
"$P/tools/taimen/mkboot.sh" "$twrp" "$w/Image" "$P/shim/out/taimen-reader.img" "$w/initramfs.cpio.gz"
