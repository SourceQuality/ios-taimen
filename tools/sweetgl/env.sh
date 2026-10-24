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
# Mesa release that mesa.patch is kept against (archive.mesa3d.org, sha256 from
# docs.mesa3d.org/relnotes/25.3.6.html)
MESA_VERSION=25.3.6
MESA_SHA256=59217efeac3b64e7ced958324b9db7494f1e0741aeb22d780276514cc1b8f206
