#!/usr/bin/env bash
# Ubuntu (WSL2) side of the ios-taimen setup (see tools/windows/README.md).
# Run from the port checkout: tools/windows/setup-wsl.sh
# Installs the shim toolchain (aarch64 gcc/binutils), avbtool, gh, and points
# ~/.local/bin/adb and ~/.local/bin/fastboot at the Windows executables.
set -euo pipefail
P=$(cd "$(dirname "$0")/../.." && pwd)
grep -qi microsoft /proc/version || { echo "not WSL; nothing to do" >&2; exit 1; }
sudo apt-get update
sudo apt-get install -y git gh python3 gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
	gzip rsync curl zstd
# avbtool: the Ubuntu package if there is one, else the AOSP script (add_hash_footer needs no key)
mkdir -p ~/.local/bin
if ! command -v avbtool >/dev/null && ! sudo apt-get install -y avbtool; then
	curl -fsSL 'https://android.googlesource.com/platform/external/avb/+/refs/heads/main/avbtool.py?format=TEXT' \
		| base64 -d > ~/.local/bin/avbtool
	chmod +x ~/.local/bin/avbtool
fi
for t in adb fastboot; do ln -sfn "$P/tools/windows/wsl-$t" ~/.local/bin/$t; done
case ":$PATH:" in *":$HOME/.local/bin:"*) ;; *) echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc; export PATH="$HOME/.local/bin:$PATH";; esac
# port/research -> ../research: tools/mkadt.py reads the iPhone X /product node from
# research/ipsw-x/d22ap.adt and only warns when it is missing
ln -sfn ../research "$P/research"
echo "== checks"
[ -x /mnt/c/platform-tools/adb.exe ] || echo "MISSING /mnt/c/platform-tools/adb.exe: run setup-windows.ps1 first"
adb version | head -1
fastboot --version | head -1
aarch64-linux-gnu-gcc --version | head -1
avbtool version
gh auth status >/dev/null 2>&1 && echo "gh: logged in" || echo "gh: run 'gh auth login' (GitHub account SourceQuality)"
