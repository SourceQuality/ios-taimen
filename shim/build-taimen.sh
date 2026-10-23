#!/bin/bash
# Build the Pixel 2 XL (taimen) shim: start.S + taimen.c + pstore.c + console.c
# (+ fdt.c loader.c, unused by stage 1), packed into a boot image that Google's
# ABL accepts for `fastboot boot` (see tools/taimen/mkboot.sh).
# A PAYLOAD (tools/mkpayload.py) may be appended to the Image at _end.
# Usage: shim/build-taimen.sh TEMPLATE_BOOT.img [PAYLOAD]   -> shim/out/${OUT_NAME:-taimen-boot}.img
set -euo pipefail
template=$(realpath "$1")
payload=${2:+$(realpath "$2")}
cd "$(dirname "$0")"
X=aarch64-linux-gnu-
command -v ${X}gcc >/dev/null || X=aarch64-elf-	# Arm GNU bare-metal toolchain
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra -DTAIMEN ${EXTRA_CFLAGS:-}"
mkdir -p out/taimen
for f in start.S taimen.c console.c fdt.c loader.c log.c ufs_taimen.c util_taimen.c; do
	${X}gcc $CFLAGS -c $f -o out/taimen/${f%.*}.o
done
# No relocation pass at runtime: only start.o's image_size may be absolute.
abs=$(${X}readelf -r out/taimen/taimen.o out/taimen/console.o out/taimen/fdt.o out/taimen/loader.o out/taimen/log.o out/taimen/ufs_taimen.o out/taimen/util_taimen.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
${X}gcc $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/taimen/start.o out/taimen/taimen.o out/taimen/console.o out/taimen/fdt.o out/taimen/loader.o out/taimen/log.o out/taimen/ufs_taimen.o out/taimen/util_taimen.o -o out/taimen/shim.elf
${X}objcopy -O binary out/taimen/shim.elf out/taimen/Image
size=$(${X}nm out/taimen/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/taimen/Image
[ -z "$payload" ] || cat "$payload" >> out/taimen/Image
../tools/taimen/mkboot.sh "$template" out/taimen/Image out/${OUT_NAME:-taimen-boot}.img
