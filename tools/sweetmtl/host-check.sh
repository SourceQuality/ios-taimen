#!/usr/bin/env bash
# Phase-1 host check of the taimen Metal stack (docs/taimen-metal-plan.md M0/M1, tools/sweetmtl/README.md):
# builds the Gallium shim, the runtime and flame's bridge for Linux x86_64 against a host Mesa build, links the
# bridge's self-test (test/mtlbridgetest.c) and runs it on Mesa's freedreno drm-shim as an Adreno 540. drm-shim
# executes nothing: the run checks that every step records, compiles its shaders with ir3 and submits, and decodes
# the submits with cffdump. Result values (what the GPU would have written) are reported as not checkable.
#
#   tools/sweetmtl/host-check.sh [--qc METALLIB] [--no-build]
#
# Inputs (defaults are the WSL layout next to this checkout):
#   RESEARCH     ../research (private: Mesa, the metal2vulkan checkout, QuartzCore's metallib)
#   MESA_SRC     $RESEARCH/m0/mesa: Mesa 26.3.0-rc2 + tools/sweetgl/mesa.patch + tools/sweetmtl/m0/mesa-a5xx-ir3.patch
#                (made by tools/sweetmtl/m0/build.sh when missing)
#   MESA_BUILD   $RESEARCH/m1/mesa-host: configured here with drm-shim and the freedreno tools
#   M2V_SRC      the metal2vulkan checkout (720ef1ce + flame's patches) that tools/sweetmtl/m2v links to
#                (default $RESEARCH/m0/metal2vulkan; tools/sweetmtl/m2v-fetch.sh makes one)
#   LLVM_AS      LLVM 21+ llvm-as for the bridge's fixtures (default: llvm-as on PATH, then ~/tools/mamba/root/envs/llvm21)
#   OUT          $RESEARCH/m1/host-check (build outputs, submit dumps)
# Toolchain: ~/tools/m0env.sh when present (gcc, meson, ninja, cargo, python3 with mako).
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
P=$(cd "$H/../.." && pwd)
RESEARCH=${RESEARCH:-$(cd "$P/.." && pwd)/research}
MESA_SRC=${MESA_SRC:-$RESEARCH/m0/mesa}
MESA_BUILD=${MESA_BUILD:-$RESEARCH/m1/mesa-host}
M2V_SRC=${M2V_SRC:-$RESEARCH/m0/metal2vulkan}
OUT=${OUT:-$RESEARCH/m1/host-check}
QC=
BUILD=1
while [ $# -gt 0 ]; do
	case $1 in
	--qc) QC=$2; shift ;;
	--no-build) BUILD=0 ;;
	*) echo "usage: $0 [--qc METALLIB] [--no-build]"; exit 2 ;;
	esac
	shift
done
[ -f ~/tools/m0env.sh ] && . ~/tools/m0env.sh
if [ -z "${LLVM_AS:-}" ]; then
	if command -v llvm-as >/dev/null; then LLVM_AS=$(command -v llvm-as); else LLVM_AS=~/tools/mamba/root/envs/llvm21/bin/llvm-as; fi
fi
export LLVM_AS
CC=${CC:-cc}
mkdir -p "$OUT"

if [ "$BUILD" = 1 ]; then
	# 1. Mesa: the M0 tree, a host build with drm-shim, the freedreno tools and the static libraries the shim links
	if [ ! -f "$MESA_SRC/src/freedreno/ir3/spv2ir3.c" ]; then
		echo "== Mesa source missing: tools/sweetmtl/m0/build.sh"
		M0=$(dirname "$MESA_SRC") "$H/m0/build.sh"
	fi
	if [ ! -f "$MESA_BUILD/build.ninja" ]; then
		echo "== configuring $MESA_BUILD"
		meson setup "$MESA_BUILD" "$MESA_SRC" -Dbuildtype=debugoptimized -Dplatforms= -Dgallium-drivers=freedreno \
			-Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dgles1=disabled \
			-Dgles2=disabled -Dopengl=true -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
			-Dlmsensors=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dshader-cache=disabled -Dexpat=disabled \
			-Dperfetto=false -Dspirv-tools=disabled -Dtools=drm-shim,freedreno -Dbuild-tests=false \
			-Dgallium-va=disabled -Dgallium-rusticl=false -Dteflon=false -Dhtml-docs=disabled \
			-Ddisplay-info=disabled > "$OUT/meson-setup.log"
	fi
	echo "== Mesa libraries, drm-shim, cffdump"
	ninja -C "$MESA_BUILD" src/gallium/drivers/freedreno/libfreedreno.a src/gallium/winsys/freedreno/drm/libfreedrenowinsys.a \
		src/gallium/auxiliary/libgallium.a src/compiler/spirv/libvtn.a src/compiler/nir/libnir.a \
		src/util/libxmlconfig.a src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so \
		src/freedreno/decode/cffdump > "$OUT/ninja.log" || { tail -20 "$OUT/ninja.log"; exit 1; }

	# 2. the Gallium shim
	echo "== Gallium shim"
	"$H/gallium/build-host.sh" "$MESA_SRC" "$MESA_BUILD" "$OUT/gallium" > "$OUT/shim.log" 2>&1 ||
		{ cat "$OUT/shim.log"; exit 1; }

	# 3. the translator checkout, the runtime and the bridge (Rust, a static library)
	if [ ! -e "$H/m2v/crates/metal2vulkan/Cargo.toml" ]; then
		echo "== linking tools/sweetmtl/m2v -> $M2V_SRC"
		M2V_SRC=$M2V_SRC "$H/m2v-fetch.sh"
	fi
	echo "== runtime and bridge (cargo, release)"
	(cd "$H/bridge" && CARGO_TARGET_DIR="$OUT/target" cargo build --release 2> "$OUT/cargo.log") ||
		{ grep -E "^(error|warning)" -A8 "$OUT/cargo.log" | head -80; exit 1; }
	NATIVE=$(cd "$H/bridge" && CARGO_TARGET_DIR="$OUT/target" cargo rustc --release --lib --crate-type staticlib -- \
		--print native-static-libs 2>&1 | sed -n 's/.*native-static-libs: //p' | tail -1)
	echo "$NATIVE" > "$OUT/native-static-libs.txt"

	# 4. the self-test tool: bridge + runtime + translator, the shim, Mesa
	echo "== mtlbridgetest"
	$CC -O1 -g -Wall -I "$H/bridge" -o "$OUT/mtlbridgetest" "$H/test/mtlbridgetest.c" \
		"$OUT/target/release/libnative_metal_bridge.a" "$OUT/gallium/libsweetgallium.a" \
		-Wl,--start-group $(cat "$OUT/gallium/mesa-libs.txt") -Wl,--end-group \
		$(pkg-config --libs libdrm zlib) -lexpat -lstdc++ $(cat "$OUT/native-static-libs.txt") \
		-Wl,--build-id=sha1 > "$OUT/link.log" 2>&1 || { tail -30 "$OUT/link.log"; exit 1; }
fi

# 5. run on drm-shim as an Adreno 540, dumping every submit
echo "== running on freedreno drm-shim, FD_GPU_ID=540"
rm -rf "$OUT/rd" "$OUT/cache" && mkdir -p "$OUT/rd"
set +e
env LD_PRELOAD="$MESA_BUILD/src/freedreno/drm-shim/libfreedreno_noop_drm_shim.so" FD_GPU_ID=540 \
	FD_RD_DUMP=combine FD_RD_DUMP_PATH="$OUT/rd" SWEETMTL_TRACE=1 SWEETMTL_DRM_SHIM=1 \
	LD_LIBRARY_PATH="${CONDA_PREFIX:-$HOME/tools/mamba/root/envs/m0}/lib:${LD_LIBRARY_PATH:-}" \
	"$OUT/mtlbridgetest" "$OUT/cache" $QC > "$OUT/run.log" 2>&1
RC=$?
set -e
cat "$OUT/run.log"

# 6. what was submitted: one .rd file per submit, decoded
echo "== submits (cffdump)"
# drm-shim leaves every submit's fence 0, which names the per-submit dumps alike, so they are combined in one
DUMP=$(ls "$OUT"/rd/*.rd* 2>/dev/null | head -1)
if [ -z "$DUMP" ]; then
	echo "no submit was dumped"
	exit 1
fi
LD_LIBRARY_PATH="${CONDA_PREFIX:-$HOME/tools/mamba/root/envs/m0}/lib:${LD_LIBRARY_PATH:-}" \
	"$MESA_BUILD/src/freedreno/decode/cffdump" --no-color "$DUMP" > "$OUT/rd/submits.txt" 2>&1 || true
awk '
	/^cmdstream\[/ { n++; ib[n] = $0 }
	/opcode: CP_EXEC_CS/ { cs[n]++ }
	/opcode: CP_DRAW_INDX/ { dr[n]++ }
	/write RB_BLIT_CNTL/ { bl[n]++ }
	/opcode: CP_LOAD_STATE4/ { ls[n]++ }
	/RB_MRT\[0\]\.BASE:/ { rt[n] = $2 }
	END {
		for (i = 1; i <= n; i++)
			printf "%-28s exec_cs %d, draws %d, blits/clears %d, load_state %d%s\n", ib[i], cs[i], dr[i], bl[i], ls[i],
			       rt[i] ? ", MRT0 at " rt[i] : ""
		printf "submits: %d (decoded in %s)\n", n, FILENAME
	}' "$OUT/rd/submits.txt"
echo "self-test exit: $RC (0 = every step ran)"
exit $RC
