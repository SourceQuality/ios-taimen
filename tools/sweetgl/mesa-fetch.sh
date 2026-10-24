#!/bin/sh
# Fetch the pinned Mesa release (env.sh) into research/mesa (or MESA_SRC),
# check its sha256 and apply tools/sweetgl/mesa.patch. The tarball is kept in
# research/mesa-dl (or MESA_DL) so a second run needs no download.
set -e
. "$(cd "$(dirname "$0")" && pwd)/env.sh"
DL=${MESA_DL:-$R/research/mesa-dl}
T=$DL/mesa-$MESA_VERSION.tar.xz
mkdir -p "$DL"
if [ ! -f "$T" ]; then
	curl -fL --retry 3 -o "$T.part" "https://archive.mesa3d.org/mesa-$MESA_VERSION.tar.xz"
	mv "$T.part" "$T"
fi
SUM=$( (shasum -a 256 "$T" 2>/dev/null || sha256sum "$T") | cut -d' ' -f1)
[ "$SUM" = "$MESA_SHA256" ] || { echo "$T: sha256 $SUM, want $MESA_SHA256"; rm -f "$T"; exit 1; }
rm -rf "$M"
mkdir -p "$M"
tar -xJf "$T" -C "$M" --strip-components 1
(cd "$M" && patch -p1 < "$H/mesa.patch")
echo "mesa-$MESA_VERSION + mesa.patch in $M"
