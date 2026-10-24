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
# The Mesa the Pixel 1 ran: mesa.patch's blob ids are those of 26.3.0-rc1/rc2
# (no 25.x or 26.0-26.2 release matches). 25.3.6 builds with two more hunks but
# calls fscreen->get_param, which sweetgl.c leaves NULL (26.x dropped it).
# sha256 of the archive.mesa3d.org tarball (rcs have no relnotes; a .sig is there).
MESA_VERSION=26.3.0-rc2
MESA_SHA256=6b9154bc46ec73901555dbcb61600eb69607dc1da718a87e7a1c09a755bb1b6d
