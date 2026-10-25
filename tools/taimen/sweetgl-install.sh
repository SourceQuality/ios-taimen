#!/usr/bin/env bash
# Make a sweetgl build (tools/sweetgl, CI workflow sweetgl.yml) the one GPU boots use:
# wait for its CI run, download the dylib, build a ROOTFIX blob that installs it in the iOS root
# as /usr/lib/sweetgl-<sha256 prefix>.dylib (write-if-absent, zlib-compressed: xnu 0245), and record both in
# ../out/sweetgl/current.env, which boot-xnu.sh reads for ROOT=1 boots (GPU=0 to boot without).
# Usage: tools/taimen/sweetgl-install.sh [SHA]      (default: the last commit touching tools/sweetgl)
set -euo pipefail
P=$(cd "$(dirname "$0")/../.." && pwd)
R=SourceQuality/ios-taimen
gh=${GH_EXE:-/mnt/c/Program Files/GitHub CLI/gh.exe}
sha=${1:-$(git -C "$P" log -1 --format=%H -- tools/sweetgl)}
id=
for i in $(seq 1 30); do
	id=$("$gh" run list -R $R -w sweetgl.yml -c "$sha" -L 1 --json databaseId -q '.[0].databaseId' 2>/dev/null | tr -d '\r')
	[ -n "$id" ] && break
	sleep 10
done
[ -n "$id" ] || { echo "no sweetgl.yml run for $sha" >&2; exit 2; }
echo "CI run $id for ${sha:0:7}"
if ! "$gh" run watch "$id" -R $R --exit-status --interval 20 > /dev/null 2>&1; then
	"$gh" run view "$id" -R $R --log-failed 2>/dev/null | tr -d '\r' | tail -40
	exit 1
fi
d=$P/../out/sweetgl/run-$id
rm -rf "$d"; mkdir -p "$d"
"$gh" run download "$id" -R $R -n sweetgl-dylib -D "$(wslpath -w "$d")"
(cd "$d" && sha256sum -c SHA256SUMS)
lib=/usr/lib/sweetgl-$(sha256sum "$d/sweetgl.dylib" | cut -c1-8).dylib
fix=$P/../research/taimen/rootfix-sweetgl-current.bin
# the phone tools (USB networking: netsvcd) ride along when tools/windows/ci-phone-tools.sh fetched them
tools=()
[ -f "$P/../out/phone-tools/netsvcd" ] && tools=(--phone-tools "$P/../out/phone-tools")
# and the RedwoodAudio HAL plug-in, iOS's speaker (tools/phone/RedwoodAudio, xnu 0323/0326)
[ -f "$P/../out/phone-tools/RedwoodAudio" ] && tools+=(--hal-plugin "$P/../out/phone-tools")
python3 -I "$P/tools/taimen/mkrootfix.py" "$P/../research/ipsw-x/ios16-root.img" "$fix" --zfile "$lib=$d/sweetgl.dylib" "${tools[@]}" | tail -2
cat > "$P/../out/sweetgl/current.env" <<EOF
SWEETGL_SHA=$sha
SWEETGL_LIB=$lib
SWEETGL_ROOTFIX=$fix
EOF
echo "GPU boots now use $lib (sweetgl ${sha:0:7}, $fix)"
