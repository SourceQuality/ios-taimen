#!/bin/bash
# Stage Apple's open source IONetworkingFamily into the SWEET kernel
# (iokit/IOKit/network). configd names network interfaces through its
# IONetworkStack and only manages interfaces that are IONetworkInterfaces.
# dxb runs patches/*.sh from the XNU source root before applying *.patch.
set -euo pipefail

[[ "${MACHINE_CONFIG:-}" =~ ^(SWEET|TAIMEN)$ ]] || exit 0
[[ "${MACOS_VERSION:-}" == 13.5 ]] || {
    echo 'SWEET IONetworkingFamily integration requires xnu-8796.141.3 (macOS 13.5)' >&2
    exit 1
}
[[ -f iokit/conf/files.arm64 ]] || exit 1

# IONetworkingFamily-170.140.2 is paired with xnu-8796.141.3 in Apple's
# macos-135 release.json. Fetch the immutable commit, never a moving branch/tag.
net_sha=f9f7e57b2132b82de09a26ddcbf56fd8125b4933
net_tmp=$(mktemp -d)
trap 'rm -rf "$net_tmp"' EXIT
git init -q "$net_tmp"
git -C "$net_tmp" fetch -q --depth 1 \
    https://github.com/apple-oss-distributions/IONetworkingFamily "$net_sha"
git -C "$net_tmp" checkout -q FETCH_HEAD
[[ "$(git -C "$net_tmp" rev-parse HEAD)" == "$net_sha" ]]

target=iokit/IOKit/network
if [[ -e "$target" ]]; then
    echo "refusing to overwrite existing sources: $target" >&2
    exit 1
fi
mkdir -p "$target"
# Sources and headers side by side: <IOKit/network/...> resolves through
# -I iokit, the family's quoted private headers next to each .cpp.
cp "$net_tmp"/*.cpp "$net_tmp"/*.h "$net_tmp"/APPLE_LICENSE "$target/"
# Kernel builds use -nostdinc; IONetworkInterface.cpp only needs the SDK's
# target macros from TargetConditionals.h.
net_sdk=$(xcrun --sdk macosx --show-sdk-path)
cp "$net_sdk/usr/include/TargetConditionals.h" "$target/"
sed -i '' 's|#include <TargetConditionals.h>|#include "TargetConditionals.h"|' "$target/IONetworkInterface.cpp"
grep -q '#include "TargetConditionals.h"' "$target/IONetworkInterface.cpp"
# Built inside xnu, <sys/mbuf.h> and friends pull in C++ (kern/startup.h
# templates): drop the family's extern "C" wrappers around plain #includes
# (the headers carry their own __BEGIN_DECLS).
python3 - "$target" <<'PY'
import re, sys
from pathlib import Path
pat = re.compile(r'extern "C" \{\n((?:[ \t]*(?:#include[^\n]*|#define[^\n]*|//[^\n]*)?\n)+)\}\n')
n = 0
for f in sorted(Path(sys.argv[1]).glob('*.[ch]*')):
    text = f.read_text(errors='surrogateescape')
    new, k = pat.subn(r'\1', text)
    if k:
        f.write_text(new, errors='surrogateescape')
        n += k
if n < 6:
    raise SystemExit(f'IONetworkingFamily source changed: unwrapped {n} extern "C" include blocks')

def fix(name, old, new):
    f = Path(sys.argv[1]) / name
    text = f.read_text(errors='surrogateescape')
    if text.count(old) != 1:
        raise SystemExit(f'IONetworkingFamily source changed: {name}')
    f.write_text(text.replace(old, new), errors='surrogateescape')

fix('IOMbufMemoryCursor.cpp',
    '__BEGIN_DECLS\n#include <IOKit/assert.h>\n\n#include <sys/param.h>\n#include <sys/mbuf.h>\n',
    '#include <IOKit/assert.h>\n#include <sys/param.h>\n#include <sys/mbuf.h>\n__BEGIN_DECLS\n')
fix('IOEthernetInterface.cpp',
    '#include <IOEthernetInterface.h>\n#include <IOEthernetController.h>\n',
    '#include "IOEthernetInterface.h"\n#include "IOEthernetController.h"\n')
# net/bpf.h has no __BEGIN_DECLS around bpfattach & co.
for name in ('IOEthernetInterface.cpp', 'IONetworkInterface.cpp'):
    fix(name, '#include <net/bpf.h>\n', 'extern "C" {\n#include <net/bpf.h>\n}\n')
# The skywalk headers that net/dlil.h brings in use LIBKERN_CONSUMED.
fix('IONetworkInterface.cpp',
    '#include <sys/param.h>\n#include <sys/systm.h>\n',
    '#include <libkern/c++/OSMetaClass.h>\n#include <sys/param.h>\n#include <sys/systm.h>\n')
PY
echo "Staged IONetworkingFamily-170.140.2 ($net_sha) for SWEET"
