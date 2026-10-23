#!/usr/bin/env bash
# Download a CI kernel (xnu.yml artifact "xnu-kernel") into out/kernels/taimen-rN.
# Usage: tools/taimen/fetch-kernel.sh N [RUN_ID]   (default run: the latest successful xnu.yml run on taimen)
set -euo pipefail
P=$(cd "$(dirname "$0")/../.." && pwd)
n=$1 R=SourceQuality/ios-taimen
id=${2:-$(gh run list -R $R -w xnu.yml -b taimen -s success -L 1 --json databaseId -q '.[0].databaseId')}
d=$P/../out/kernels/taimen-r$n
mkdir -p "$d"
gh run download "$id" -R $R -n xnu-kernel -D "$d"
ls -l "$d"
