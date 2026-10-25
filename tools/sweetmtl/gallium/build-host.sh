#!/usr/bin/env bash
# Build the Gallium shim (sg_*.c) as a static library against a Mesa build tree: the host's
# (tools/sweetmtl/host-check.sh) or the iOS cross build's (tools/sweetmtl/build-ios.sh, which
# passes the iPhoneOS clang as CC):
#   tools/sweetmtl/gallium/build-host.sh MESA_SRC MESA_BUILD OUT_DIR [EXTRA.c ...]
# The compiler flags (include paths, Mesa's configure-time defines) are those Mesa's own build
# gives freedreno_resource.c (compile_commands.json), as tools/sweetgl/build.sh takes the state
# tracker's. EXTRA.c files (iOS: tools/sweetgl/sweet_drm.c) are compiled the same way and go into
# the library too. Output: OUT_DIR/libsweetgallium.a and OUT_DIR/mesa-libs.txt (the Mesa static
# libraries to link after it, in order, as far as this build has them).
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
M=$(cd "$1" && pwd)
B=$(cd "$2" && pwd)
O=$3
shift 3
mkdir -p "$O"
CC=${CC:-cc}
AR=${AR:-ar}
python3 -I - "$B/compile_commands.json" "$O/mesa-flags.rsp" <<'PY'
import json, shlex, sys, os
cc = json.load(open(sys.argv[1]))
e = [x for x in cc if x["file"].endswith("drivers/freedreno/freedreno_resource.c")][0]
d = e["directory"]
args = shlex.split(e["command"])
out = []
i = 0
while i < len(args):
    a = args[i]
    if a.startswith("-I"):
        p = a[2:]
        out.append("-I" + os.path.normpath(os.path.join(d, p)))
    elif a.startswith("-D") or a.startswith("-std=") or a.startswith("-f") and a in ("-fno-math-errno", "-fno-trapping-math", "-fvisibility=hidden"):
        out.append(a)
    elif a == "-include":
        out += ["-include", os.path.normpath(os.path.join(d, args[i + 1]))]
        i += 1
    i += 1
open(sys.argv[2], "w").write("\n".join(shlex.quote(x) for x in out) + "\n")
PY
EXTRA="-I$M/src/gallium/drivers/freedreno -I$M/src/freedreno -I$M/src/freedreno/common
	-I$M/src/gallium/auxiliary -I$M/src/gallium/include -I$M/src/gallium/winsys -I$M/include
	-I$M/src -I$B/src -I$B/src/freedreno/common -I$B/src/compiler/nir -I$B/src/compiler/spirv
	-I$M/src/compiler/nir -I$M/src/compiler -I$B/src/compiler -I$M/src/vulkan/util
	-I$M/src/gallium/drivers -I$H/../../sweetgl"
# softpipe, the PC's validation driver (SG_DRIVER=softpipe), where the Mesa build has it
SOFTPIPE=
SOFTPIPE_LIBS=
if [ -f "$B/src/gallium/drivers/softpipe/libsoftpipe.a" ] && [ -f "$B/src/gallium/winsys/sw/null/libws_null.a" ]; then
	SOFTPIPE=-DSG_WITH_SOFTPIPE
	SOFTPIPE_LIBS="$B/src/gallium/drivers/softpipe/libsoftpipe.a $B/src/gallium/winsys/sw/null/libws_null.a"
fi
OBJS=
for f in "$H/sg_device.c" "$H/sg_resource.c" "$H/sg_shader.c" "$H/sg_replay.c" "$H/sg_format.c" \
	"$H/sg_map.c" "$H/sg_iosurface.c" "$@"; do
	o=$O/$(basename "$f" .c).o
	$CC -c -O2 -g -fPIC -Wall -Wno-unused-function -Wno-missing-field-initializers \
		@"$O/mesa-flags.rsp" $EXTRA $SOFTPIPE -o "$o" "$f"
	OBJS="$OBJS $o"
done
rm -f "$O/libsweetgallium.a"
$AR rcs "$O/libsweetgallium.a" $OBJS
# Mesa's static libraries for the shim, in link order (the gallium drivers first). The clflush
# helpers are x86 only.
for l in $SOFTPIPE_LIBS \
	"$B/src/gallium/winsys/freedreno/drm/libfreedrenowinsys.a" \
	"$B/src/gallium/drivers/freedreno/libfreedreno.a" \
	"$B/src/freedreno/drm/libfreedreno_drm.a" \
	"$B/src/freedreno/ir3/libfreedreno_ir3.a" \
	"$B/src/freedreno/ir2/libfreedreno_ir2.a" \
	"$B/src/freedreno/fdl/libfreedreno_layout.a" \
	"$B/src/freedreno/common/libfreedreno_common.a" \
	"$B/src/freedreno/perfcntrs/libfreedreno_perfcntrs.a" \
	"$B/src/freedreno/isa/libir3encode.a" \
	"$B/src/freedreno/isa/libir3decode.a" \
	"$B/src/compiler/isaspec/libisaspec.a" \
	"$B/src/gallium/auxiliary/libgallium.a" \
	"$B/src/compiler/spirv/libvtn.a" \
	"$B/src/compiler/nir/libnir.a" \
	"$B/src/compiler/libcompiler.a" \
	"$B/src/util/libxmlconfig.a" \
	"$B/src/util/libmesa_util.a" \
	"$B/src/util/libmesa_util_simd.a" \
	"$B/src/util/libmesa_util_clflush.a" \
	"$B/src/util/libmesa_util_clflushopt.a" \
	"$B/src/util/blake3/libblake3.a" \
	"$B/src/c11/impl/libmesa_util_c11.a"; do
	if [ -f "$l" ]; then echo "$l"; fi
done > "$O/mesa-libs.txt"
echo "$O/libsweetgallium.a"
