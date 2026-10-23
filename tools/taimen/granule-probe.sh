#!/usr/bin/env bash
# Pack the Pixel 1 port's 16K-page probe (port/tools/granule-probe.S) into a
# boot image that the Pixel 2 XL (taimen) ABL accepts for `fastboot boot`.
#
# taimen ABL rules, measured from TWRP 3.7.0 and the LineageOS boot_a (both
# boot via `fastboot boot`): header v0, 4096-byte pages, compressed kernel
# (gzip or LZ4 frame) with the DTB appended, and an AVB hash footer padding
# the image to the 40 MiB boot partition.  mkboot-v0.sh does the first part
# (gzip + stock DTB kept from the template); avbtool adds the footer.
#
# The probe answers through PSCI: 16K granule supported -> phone reboots at
# once; unsupported -> phone powers off; hang on the Google logo -> the probe
# never ran (image rejected or jump failed).
#
# Usage: tools/granule-probe-taimen.sh [TEMPLATE_BOOT.img] [OUT.img]
set -euo pipefail
P=$(cd "$(dirname "$0")/../.." && pwd)
template=${1:-${TEMPLATE:-$P/../backup/boot_a.img}}
out=${2:-$P/out/taimen/granule-probe.img}
mkdir -p "$(dirname "$out")"
X=aarch64-linux-gnu-
command -v ${X}gcc >/dev/null || X=aarch64-elf-
command -v ${X}gcc >/dev/null || { echo "no aarch64 cross gcc (aarch64-linux-gnu-gcc or aarch64-elf-gcc)" >&2; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
${X}gcc -nostdlib -static -Wl,-Ttext=0 -Wl,--build-id=none \
	"${PROBE_SRC:-$P/tools/taimen/granule-probe-fb.S}" -o "$tmp/probe.elf"
${X}objcopy -O binary "$tmp/probe.elf" "$tmp/Image"
"$P/tools/mkboot-v0.sh" "$template" "$tmp/Image" "$tmp/boot.img"
cp "$tmp/boot.img" "$out"
# Same footer shape as TWRP's image: hash footer, 40 MiB partition, name "boot".
avbtool add_hash_footer --image "$out" --partition_size 41943040 --partition_name boot
avbtool info_image --image "$out" | grep -E 'Image size|Original image size|Partition Name'
echo "$out ready. Phone in fastboot, then:  fastboot boot $out"
