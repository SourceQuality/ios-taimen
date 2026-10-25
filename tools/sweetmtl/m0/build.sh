#!/usr/bin/env bash
# M0 of docs/taimen-metal-plan.md, on Linux x86_64 (WSL): build the host tools.
#   - Mesa (the sweetgl pin, tools/sweetgl/env.sh) + mesa.patch + mesa-a5xx-ir3.patch, with
#     spv2ir3 in src/freedreno/ir3, as a host build in $M0/mesa-host;
#   - m0dump (needs the metal2vulkan checkout in $M0/metal2vulkan; its CLI is built there with
#     cargo build --release --features cli).
# M0 defaults to ../research/m0 next to the port checkout. Needs meson, ninja, flex, bison,
# python3 with mako and pyyaml, a C compiler, cargo; libdrm and zlib headers.
#   tools/sweetmtl/m0/build.sh
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
P=$(cd "$H/../../.." && pwd)
M0=${M0:-$(cd "$P/.." && pwd)/research/m0}
mkdir -p "$M0"
export MESA_SRC=$M0/mesa
if [ ! -f "$MESA_SRC/src/freedreno/ir3/spv2ir3.c" ]; then
	sh "$P/tools/sweetgl/mesa-fetch.sh"
	(cd "$MESA_SRC" && patch -p1 < "$H/mesa-a5xx-ir3.patch")
	cat >> "$MESA_SRC/src/freedreno/ir3/meson.build" <<'EOF'

# M0 (taimen Metal plan): metal2vulkan SPIR-V through ir3 for a5xx, bindful.
spv2ir3 = executable(
  'spv2ir3',
  'spv2ir3.c',
  link_with: [libfreedreno_ir3, libfreedreno_common],
  dependencies: [idep_libir3decode, idep_mesautil, idep_nir, idep_vtn],
  include_directories: [inc_freedreno, inc_include, inc_src],
  build_by_default: false,
)
EOF
fi
cp "$H/spv2ir3.c" "$MESA_SRC/src/freedreno/ir3/spv2ir3.c"
if [ ! -f "$M0/mesa-host/build.ninja" ]; then
	meson setup "$M0/mesa-host" "$MESA_SRC" -Dbuildtype=debugoptimized -Dplatforms= \
		-Dgallium-drivers=freedreno -Dvulkan-drivers= -Dglx=disabled -Degl=disabled \
		-Dgbm=disabled -Dgles1=disabled -Dgles2=disabled -Dllvm=disabled -Dvalgrind=disabled \
		-Dlibunwind=disabled -Dlmsensors=disabled -Dzstd=disabled -Dxmlconfig=disabled \
		-Dshader-cache=disabled -Dexpat=disabled -Dperfetto=false -Dspirv-tools=disabled
fi
ninja -C "$M0/mesa-host" src/freedreno/ir3/spv2ir3 src/freedreno/isa/ir3-disasm
(cd "$H/m0dump" && CARGO_TARGET_DIR=$M0/m0dump-target cargo build --release)
echo "spv2ir3: $M0/mesa-host/src/freedreno/ir3/spv2ir3"
echo "m0dump:  $M0/m0dump-target/release/m0dump"
