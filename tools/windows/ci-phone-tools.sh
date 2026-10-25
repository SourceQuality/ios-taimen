#!/usr/bin/env bash
# Download the latest successful phone-tools CI build (tools/phone: netsvcd, sweetexc.dylib, ...)
# into ../out/phone-tools, with Windows' gh.exe. tools/taimen/sweetgl-install.sh and
# mkrootfix.py --phone-tools install them into the iOS root.
#   tools/windows/ci-phone-tools.sh
set -euo pipefail
P=$(cd "$(dirname "$0")/../.." && pwd)
R=SourceQuality/ios-taimen
gh=${GH_EXE:-/mnt/c/Program Files/GitHub CLI/gh.exe}
id=$("$gh" run list -R $R -w phone-tools.yml -s success -L 1 --json databaseId -q '.[0].databaseId' | tr -d '\r')
[ -n "$id" ] || { echo "no successful phone-tools run" >&2; exit 1; }
d=$P/../out/phone-tools
rm -rf "$d"; mkdir -p "$d"
"$gh" run download "$id" -R $R -n phone-tools -D "$(wslpath -w "$d")"
(cd "$d" && sha256sum -c SHA256SUMS)
cat "$d/BUILDINFO.txt"
