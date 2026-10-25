#!/bin/sh
# The metal2vulkan translator at tools/sweetmtl/m2v (docs/taimen-metal-plan.md 1.4): the pin flame built on,
# 720ef1ce, with flame's translator patches (tools/sweetmtl/patches: 0001, 0004, 0005, from
# ref/ios-flame/phone/metal2/bridge/patches; LGPL-3.0-or-later OR GPL-2.0-or-later like the code they change).
#   tools/sweetmtl/m2v-fetch.sh                  clone, check out the pin, apply the patches
#   M2V_SRC=/path tools/sweetmtl/m2v-fetch.sh    link an existing checkout instead (WSL: research/m0/metal2vulkan,
#                                                 the one M0 used, which has flame's six patches applied)
# Only the translator crates are built (metal2vulkan, metal2vulkan-air); flame's runtime patches do not matter here.
set -e
H=$(cd "$(dirname "$0")" && pwd)
M2V_REPO=${M2V_REPO:-https://github.com/steelbrain/metal2vulkan}
M2V_PIN=${M2V_PIN:-720ef1ce}
D=$H/m2v
if [ -n "$M2V_SRC" ]; then
	[ -f "$M2V_SRC/crates/metal2vulkan/Cargo.toml" ] || { echo "$M2V_SRC is not a metal2vulkan checkout"; exit 1; }
	rm -f "$D"
	ln -s "$M2V_SRC" "$D"
	echo "tools/sweetmtl/m2v -> $M2V_SRC"
	exit 0
fi
if [ ! -d "$D/.git" ]; then
	rm -rf "$D"
	git clone --filter=blob:none "$M2V_REPO" "$D"
fi
cd "$D"
git checkout -q "$M2V_PIN"
for p in "$H"/patches/*.patch; do
	[ -e "$p" ] || continue
	if git apply --check "$p" 2>/dev/null; then
		git apply "$p"
		echo "applied $(basename "$p")"
	else
		echo "skipped $(basename "$p") (applied already, or does not apply)"
	fi
done
echo "metal2vulkan $M2V_PIN in $D"
