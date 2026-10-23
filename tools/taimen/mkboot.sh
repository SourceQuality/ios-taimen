#!/bin/bash
# Wrap a raw arm64 Image for the Pixel 2 XL (taimen) ABL's `fastboot boot`:
# v0 header and appended DTB kept from the template (tools/mkboot-v0.sh,
# gzip kernel), then an AVB hash footer padding to the 40 MiB boot partition,
# the shape TWRP and LineageOS images have. Measured rules: NOTES-taimen.md.
# Usage: tools/taimen/mkboot.sh TEMPLATE_BOOT.img Image OUT.img
set -euo pipefail
template=$1 image=$2 out=$3
"$(dirname "$0")/../mkboot-v0.sh" "$template" "$image" "$out"
avbtool add_hash_footer --image "$out" --partition_size 41943040 --partition_name boot
avbtool info_image --image "$out" | grep -E 'Image size|Original image size'
echo "$out: fastboot boot $out"
