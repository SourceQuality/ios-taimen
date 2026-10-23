#!/usr/bin/env bash
# On the old Linux laptop: pack what the Windows/WSL setup needs and git does not hold
# (tools/windows/README.md step 2) into ~/Projects/ios-taimen/transfer/. Private: holds the
# partition backups (serial, IMEI). Usage: tools/windows/make-transfer-bundle.sh [SCRATCH_DIR]
set -euo pipefail
T=$(cd "$(dirname "$0")/../../.." && pwd)	# ~/Projects/ios-taimen
scratch=${1:-}
dst=$T/transfer; mkdir -p "$dst" "$T/handoff"
cdir=~/.claude/projects/$(dirname "$T" | tr / -)	# Claude Code project key of ~/Projects
# handoff/: the patch-authoring xnu tree, Claude memory + transcripts of the Linux sessions
if [ -n "$scratch" ] && [ -d "$scratch/xnu/.git" ]; then
	git -C "$scratch/xnu" bundle create "$T/handoff/xnu-scratch.bundle" --all
	tar -C "$scratch" -cf "$T/handoff/dxb.tar" dxb
fi
rm -rf "$T/handoff/claude-memory"; cp -r "$cdir"/memory "$T/handoff/claude-memory" 2>/dev/null || true
mkdir -p "$T/handoff/claude-transcripts"
grep -l taimen "$cdir"/*.jsonl 2>/dev/null | xargs -r cp -t "$T/handoff/claude-transcripts/"	# taimen sessions only
cd "$T"
ipsw=(research/ipsw-x/{URL.txt,meta.json,mkiosroot.log,dt,d22ap.adt,appos,ios16-root-tail.simg})
tar --zstd -cf "$dst/ios-taimen-private.tar.zst" backup research/taimen "${ipsw[@]}" hw tools \
	out/kernels/taimen-r22 out/taimen-xnu-r22 handoff
[ -n "${NO_ROOT_IMAGE:-}" ] || cp --reflink=auto -u research/ipsw-x/ios16-root.simg "$dst/"
(cd "$dst" && sha256sum ios-taimen-private.tar.zst $( [ -f ios16-root.simg ] && echo ios16-root.simg ) > SHA256SUMS)
ls -lh "$dst"
