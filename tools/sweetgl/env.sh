# Sourced by mesa-fetch.sh, mesa-setup.sh and build.sh: where Mesa and the
# toolchain are. Defaults are the Mac layout under research/; the GitHub job
# (.github/workflows/sweetgl.yml) keeps them and only sets DEVELOPER_DIR.
#   MESA_SRC     patched Mesa tree (default research/mesa)
#   MESA_BUILD   meson build dir (default research/mesa-ios)
#   DEVELOPER_DIR  Xcode (default research/xcode15 if unpacked, else xcode-select)
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../.." && pwd)
M=${MESA_SRC:-$R/research/mesa}
B=${MESA_BUILD:-$R/research/mesa-ios}
if [ -z "$DEVELOPER_DIR" ] && [ -d "$R/research/xcode15/Xcode.app" ]; then
	export DEVELOPER_DIR=$R/research/xcode15/Xcode.app/Contents/Developer
fi
export PATH=$R/research/tools/venv/bin:$R/research/tools/bin:$PATH
# Mesa pin: 26.3.0-rc2, the tree the Pixel 1 built and ran sweetgl.dylib on.
# Do not "upgrade" it to a release without re-checking mesa.patch, sweetgl.c
# and eagl.c against the new tree. Evidence: the index lines of mesa.patch are
# the pre-patch blobs of exactly that tree,
#   src/freedreno/drm/freedreno_bo.c                   6eda810
#   src/freedreno/meson.build                          e8f01b8
#   src/gallium/drivers/freedreno/freedreno_resource.c 29134ba
# (26.3.0-rc1 has the same three; 25.3.6, 26.0.0, 26.1.0, 26.2.0 and 26.2.5 do
# not), and the patch applies to it without offsets. 25.3.6 (the plan's first
# pick) builds only with two more hunks (no Mach-O build id, sys/sysinfo.h) and
# then crashes: its st_api_create_context calls fscreen->get_param
# (st_manager.c:1037), which sweetgl.c leaves NULL because 26.x replaced it
# with bool broken_invalidate. sha256 of the archive.mesa3d.org tarball
# (rcs have no relnotes; a .sig is published next to it).
MESA_VERSION=26.3.0-rc2
MESA_SHA256=6b9154bc46ec73901555dbcb61600eb69607dc1da718a87e7a1c09a755bb1b6d
