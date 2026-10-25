#!/usr/bin/env bash
# The ROOTFIX blob for one sweetmtl phone test (tools/sweetmtl/README.md, "Install and boot"): fetch a sweetmtl CI
# build (workflow sweetmtl.yml), check its SHA256SUMS, build ../research/taimen/rootfix-sweetmtl-TOOL.bin with
# mkrootfix.py (base image ../research/ipsw-x/ios16-root.img), record it in ../out/sweetmtl/current.env and print the
# boot-args and the boot-xnu.sh line. It does not boot the phone.
#
#   tools/taimen/sweetmtl-install.sh [-r RUN_ID | -c SHA] [-p] [TOOL]
#
#   TOOL  mtltest (default; M1/M2): the SweetMetal plug-in as /System/Library/Extensions/SweetMetal-<hash8>.bundle,
#         the clients file /private/var/sweetmtl/clients (rewritten each boot: mtltest only) and mtltest as a
#         --daemon job; boot-args taimenmtl=SweetMetal-<hash8> sweetlogp=mtltest (kernel with xnu 0329).
#         mtlbridgetest or mtlabitest: those tools as --daemon jobs (no plug-in, no taimenmtl).
#   -r    that CI run; -c the sweetmtl.yml run of that commit (waited for); default: the newest successful run on taimen
#   -p    print the mkrootfix.py command only
#
# The download stays in ../out/sweetmtl/run-ID/ and is reused. The phone tools (netsvcd) and the RedwoodAudio HAL
# plug-in ride along when ../out/phone-tools has them, as tools/taimen/sweetgl-install.sh passes them: mkrootfix
# rewrites launchd.plist from the root image, so netsvcd's job must be in every blob. sweetgl's dylib is not in the
# blob (it is on the phone already): boot with ROOTFIX_KEEP=1.
set -euo pipefail
P=$(cd "$(dirname "$0")/../.." && pwd)
R=SourceQuality/ios-taimen
# Windows' gh.exe (signed in; WSL's gh is not), as tools/windows/ci-kernel.sh uses it
gh=${GH_EXE:-/mnt/c/Program Files/GitHub CLI/gh.exe}
run=
sha=
print_only=0
while getopts r:c:p o; do
	case $o in
	r) run=$OPTARG ;;
	c) sha=$OPTARG ;;
	p) print_only=1 ;;
	*) sed -n 2,16p "$0" >&2; exit 2 ;;
	esac
done
shift $((OPTIND - 1))
tool=${1:-mtltest}
case $tool in
mtltest | mtlbridgetest | mtlabitest) ;;
*) echo "TOOL is mtltest, mtlbridgetest or mtlabitest" >&2; exit 2 ;;
esac

if [ -z "$run" ] && [ -n "$sha" ]; then
	for i in $(seq 1 30); do
		run=$("$gh" run list -R $R -w sweetmtl.yml -c "$sha" -L 1 --json databaseId -q '.[0].databaseId' 2>/dev/null | tr -d '\r')
		[ -n "$run" ] && break
		sleep 10
	done
	[ -n "$run" ] || { echo "no sweetmtl.yml run for $sha" >&2; exit 2; }
	echo "CI run $run for ${sha:0:7}: waiting for it (60 min at most)"
	if ! timeout 3600 "$gh" run watch "$run" -R $R --exit-status --interval 20 > /dev/null 2>&1; then
		"$gh" run view "$run" -R $R --log-failed 2>/dev/null | tr -d '\r' | tail -40
		exit 1
	fi
elif [ -z "$run" ]; then
	run=$("$gh" run list -R $R -w sweetmtl.yml -b taimen -s success -L 1 --json databaseId -q '.[0].databaseId' | tr -d '\r')
	[ -n "$run" ] || { echo "no successful sweetmtl.yml run on taimen" >&2; exit 2; }
fi

d=$P/../out/sweetmtl/run-$run
if ! (cd "$d" 2>/dev/null && sha256sum -c --quiet SHA256SUMS > /dev/null 2>&1); then
	rm -rf "$d"
	mkdir -p "$d"
	dl=$d
	case $gh in *.exe) dl=$(wslpath -w "$d") ;; esac
	echo "downloading the sweetmtl artifact of run $run into $d"
	timeout 900 "$gh" run download "$run" -R $R -n sweetmtl -D "$dl"
fi
(cd "$d" && sha256sum -c --quiet SHA256SUMS)
h8() { sha256sum "$1" | cut -c1-8; }

args=()
bundle=
case $tool in
mtltest)
	for f in mtltest clients SweetMetal.bundle/SweetMetal SweetMetal.bundle/Info.plist SweetMetal.bundle/_CodeSignature/CodeResources; do
		[ -f "$d/$f" ] || { echo "run $run has no $f (a build before M1?)" >&2; exit 2; }
	done
	# backboardd and SpringBoard stay on GLES (sweetgl) until M5: never give them the plug-in's device here
	if grep -qE '^[[:space:]]*(backboardd|SpringBoard)[[:space:]]*(#.*)?$' "$d/clients"; then
		echo "$d/clients lists backboardd or SpringBoard: refusing" >&2
		exit 2
	fi
	MH=$(h8 "$d/SweetMetal.bundle/SweetMetal")
	TH=$(h8 "$d/mtltest")
	bundle=SweetMetal-$MH
	X=/System/Library/Extensions/$bundle.bundle
	args+=(--zfile "$X/SweetMetal=$d/SweetMetal.bundle/SweetMetal"
		--zfile "$X/Info.plist=$d/SweetMetal.bundle/Info.plist"
		--zfile "$X/_CodeSignature/CodeResources=$d/SweetMetal.bundle/_CodeSignature/CodeResources"
		--mkdir /private/var/sweetmtl
		--write "/private/var/sweetmtl/clients=$d/clients"
		--zfile "/usr/local/libexec/mtltest-$TH/mtltest=$d/mtltest"
		--daemon "com.sweet.mtltest=/usr/local/libexec/mtltest-$TH/mtltest,--daemon")
	bootargs="taimenmtl=$bundle sweetlogp=mtltest"
	;;
mtlbridgetest)
	BH=$(h8 "$d/mtlbridgetest")
	args+=(--zfile "/usr/local/libexec/mtlbridgetest-$BH/mtlbridgetest=$d/mtlbridgetest"
		--daemon "com.sweet.mtlbridgetest=/usr/local/libexec/mtlbridgetest-$BH/mtlbridgetest,--daemon,/private/var/sweetmtl/cache,/System/Library/Frameworks/QuartzCore.framework/default.metallib")
	bootargs="sweetlogp=mtlbridgetest"
	;;
mtlabitest)
	AH=$(h8 "$d/mtlabitest")
	args+=(--zfile "/usr/local/libexec/mtlabitest-$AH/mtlabitest=$d/mtlabitest"
		--daemon "com.sweet.mtlabitest=/usr/local/libexec/mtlabitest-$AH/mtlabitest,--daemon")
	bootargs="sweetlogp=mtlabitest"
	;;
esac

tools=()
[ -f "$P/../out/phone-tools/netsvcd" ] && tools=(--phone-tools "$P/../out/phone-tools")
[ -f "$P/../out/phone-tools/RedwoodAudio" ] && tools+=(--hal-plugin "$P/../out/phone-tools")
img=$P/../research/ipsw-x/ios16-root.img
fix=$P/../research/taimen/rootfix-sweetmtl-$tool.bin
cmd=(python3 -I "$P/tools/taimen/mkrootfix.py" "$img" "$fix" "${tools[@]}" "${args[@]}")
printf '%q ' "${cmd[@]}"
echo
if [ "$print_only" = 1 ]; then
	echo "boot-args: $bootargs"
	exit 0
fi
mkdir -p "$(dirname "$fix")"
"${cmd[@]}" | tail -3
size=$(stat -c %s "$fix")
echo "$fix: $((size / 1024)) KiB"
# the boot payload holds the kernel (~17 MB for a development one) and this blob: ABL stops near 35 MiB
[ "$size" -lt $((17 * 1024 * 1024)) ] || echo "WARNING: a blob this size may push the payload past ABL's ~35 MiB" >&2
mkdir -p "$P/../out/sweetmtl"
cat > "$P/../out/sweetmtl/current.env" << EOF
SWEETMTL_RUN=$run
SWEETMTL_TOOL=$tool
SWEETMTL_BUNDLE=$bundle
SWEETMTL_ROOTFIX=$fix
SWEETMTL_BOOTARGS="$bootargs"
EOF
echo "boot-args: $bootargs"
echo "boot (one tool per boot; a GPU boot, kernel with xnu 0329 for mtltest):"
echo "  ROOT=1 ROOTFIX=$fix ROOTFIX_KEEP=1 tools/taimen/boot-xnu.sh KERNEL OUTDIR $bootargs"
