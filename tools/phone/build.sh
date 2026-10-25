#!/bin/bash
# Build the on-phone tools into out/ with Xcode's iPhoneOS SDK (macOS: the phone-tools CI job),
# signed ad hoc, with NAME-ents.plist when there is one. Install them with mkrootfix.py --zfile
# (and --daemon for the launchd jobs).
#   netsvcd   USB Ethernet as iOS's primary network service (launchd job com.sweet.netsvcd)
#   sweetexc.dylib  prints why a process dies of an uncaught exception (xnu 0311 inserts it)
#   tcctest   what tccd answers an entitled platform process (launchd job com.sweet.tcctest)
#   sweettcc.dylib  TCCAccessPreflight that agrees with tccd's audit token check (xnu 0312 inserts it)
#   RedwoodAudio (+ RedwoodAudio.plist)  the HAL plug-in for mediaserverd's speaker (RedwoodAudio/)
set -euo pipefail
cd "$(dirname "$0")"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="$(xcrun --sdk iphoneos --find clang) -isysroot $SDK -arch arm64 -miphoneos-version-min=16.0"
mkdir -p out
build() {	# NAME SOURCE [FLAGS...]
	local name=$1 src=$2
	shift 2
	$CC -O2 -Wall -Werror "$@" -o "out/$name" "$src"
	if [ -f "$name-ents.plist" ]; then
		codesign -f -s - --entitlements "$name-ents.plist" "out/$name"
	else
		codesign -f -s - "out/$name"
	fi
}
build netsvcd netsvcd.c -framework CoreFoundation -framework SystemConfiguration
build sweetexc.dylib sweetexc.m -dynamiclib -install_name /usr/local/lib/sweetexc.dylib -fobjc-arc -framework Foundation
build tcctest tcctest.c -framework CoreFoundation
build sweettcc.dylib sweettcc.c -dynamiclib -install_name /usr/local/lib/sweettcc.dylib TCC.tbd -framework CoreFoundation
# RedwoodAudio: the HAL plug-in in mediaserverd (RedwoodAudio/README.md; HackiOS, MIT): the bundle
# executable and its Info.plist, which mkrootfix.py --hal-plugin installs
$CC -O2 -Wall -bundle -framework CoreFoundation -o out/RedwoodAudio RedwoodAudio/audio_plugin.c
codesign -f -s - out/RedwoodAudio
cp RedwoodAudio/audio_plugin.plist out/RedwoodAudio.plist
