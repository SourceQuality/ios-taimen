#!/bin/bash
# Build the on-phone tools into out/ with Xcode's iPhoneOS SDK (macOS: the phone-tools CI job),
# signed ad hoc, with NAME-ents.plist when there is one. Install them with mkrootfix.py --zfile
# (and --daemon for the launchd jobs).
#   netsvcd   USB Ethernet as iOS's primary network service (launchd job com.sweet.netsvcd)
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
