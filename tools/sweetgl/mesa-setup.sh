#!/bin/sh
# Configure Mesa (research/mesa from mesa-fetch.sh, with tools/sweetgl/mesa.patch)
# for iOS 16 arm64: gallium freedreno + GLES, no window system, no LLVM ->
# research/mesa-ios. The toolchain and SDK are the iphoneos ones xcrun finds.
set -e
. "$(cd "$(dirname "$0")" && pwd)/env.sh"
CLANG=$(xcrun --sdk iphoneos --find clang)
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
WRAP=
command -v ccache >/dev/null && WRAP="'ccache'"
mkdir -p "$H/out"
printf "[constants]\ntoolchain = '%s'\nsdk = '%s'\nsweetgl = '%s'\ncc_wrapper = [%s]\n" \
	"$(dirname "$CLANG")" "$SDK" "$H" "$WRAP" > "$H/out/ios-paths.txt"
cat "$H/out/ios-paths.txt"
cd "$M"
rm -rf "$B"
meson setup "$B" --cross-file "$H/out/ios-paths.txt" --cross-file "$H/ios-cross.txt" \
	--buildtype=release -Db_ndebug=true -Ddefault_library=static \
	-Dplatforms= -Degl=disabled -Dglx=disabled -Dgbm=disabled \
	-Dgallium-drivers=freedreno -Dvulkan-drivers= -Dfreedreno-kmds=msm \
	-Dllvm=disabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dzlib=disabled -Dexpat=disabled -Dxmlconfig=disabled \
	-Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled \
	-Dbuild-tests=false -Dshader-cache=disabled -Dperfetto=false \
	-Dgallium-va=disabled -Dgallium-rusticl=false -Dtools= \
	-Dhtml-docs=disabled -Ddisplay-info=disabled -Dteflon=false "$@"
