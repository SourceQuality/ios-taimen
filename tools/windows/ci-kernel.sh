#!/usr/bin/env bash
# Wait for the xnu.yml CI run of the last commit that touched xnu/patches (the pushes that
# build) and download its kernel into ../out/kernels/taimen-rN, with Windows' gh.exe (signed
# in there; WSL's gh need not be).
# Usage: tools/windows/ci-kernel.sh N     (exit 1: the build failed, its log tail is printed)
set -euo pipefail
P=$(cd "$(dirname "$0")/../.." && pwd)
n=$1 R=SourceQuality/ios-taimen
gh=${GH_EXE:-/mnt/c/Program Files/GitHub CLI/gh.exe}
sha=$(git -C "$P" log -1 --format=%H -- xnu/patches)
id=
for i in $(seq 1 30); do
	id=$("$gh" run list -R $R -w xnu.yml -c "$sha" -L 1 --json databaseId -q '.[0].databaseId' 2>/dev/null | tr -d '\r')
	[ -n "$id" ] && break
	sleep 10
done
[ -n "$id" ] || { echo "no xnu.yml run for $sha" >&2; exit 2; }
echo "CI run $id for ${sha:0:7}"
if ! "$gh" run watch "$id" -R $R --exit-status --interval 20 > /dev/null 2>&1; then
	"$gh" run view "$id" -R $R --log-failed 2>/dev/null | tr -d '\r' | tail -40
	exit 1
fi
d=$P/../out/kernels/taimen-r$n
rm -rf "$d"; mkdir -p "$d"   # gh refuses to overwrite an earlier download
"$gh" run download "$id" -R $R -n xnu-kernel -D "$(wslpath -w "$d")"
(cd "$d" && sha256sum -c SHA256SUMS)
