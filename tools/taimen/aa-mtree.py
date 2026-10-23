#!/usr/bin/env python3
"""Decode an IPSW *.dmg.mtree (IM4P -> pbze-chunked LZFSE -> Apple Archive) into a TSV manifest:
   path \\t type \\t mode(octal) \\t uid \\t gid \\t size \\t link-target
Usage: aa-mtree.py UNWRAPPED.mtree OUT.tsv      (UNWRAPPED = tools/adt.py unwrap output, starts with 'pbze')
"""
import struct, subprocess, sys

raw = open(sys.argv[1], 'rb').read()
assert raw[:4] == b'pbze', 'expected a pbze-chunked payload'
# concatenated LZFSE frames ("bvx2" ... "bvx$"): decode each with the lzfse CLI
frames, i = [], raw.find(b'bvx2')
while i >= 0:
    j = raw.find(b'bvx$', i)
    assert j > 0
    frames.append(raw[i:j + 4])
    i = raw.find(b'bvx2', j + 4)
aa = b''
for fr in frames:
    aa += subprocess.run(['lzfse', '-decode'], input=fr, capture_output=True, check=True).stdout
print(f'{len(frames)} frames -> {len(aa)} bytes of Apple Archive', file=sys.stderr)

out = open(sys.argv[2], 'w')
pos, n = 0, 0
while pos + 6 <= len(aa):
    if aa[pos:pos + 4] != b'AA01':
        raise SystemExit(f'bad header at {pos:#x}: {aa[pos:pos+8]!r}')
    hsize = struct.unpack_from('<H', aa, pos + 4)[0]
    p, end = pos + 6, pos + hsize
    f = {}
    blob_after = 0
    while p < end:
        key, t = aa[p:p + 3].decode(), chr(aa[p + 3]); p += 4
        if t == '1': f[key] = aa[p]; p += 1
        elif t == '2': f[key] = struct.unpack_from('<H', aa, p)[0]; p += 2
        elif t == '4': f[key] = struct.unpack_from('<I', aa, p)[0]; p += 4
        elif t == '8': f[key] = struct.unpack_from('<Q', aa, p)[0]; p += 8
        elif t == 'P': ln = struct.unpack_from('<H', aa, p)[0]; f[key] = aa[p + 2:p + 2 + ln].decode(errors='replace'); p += 2 + ln
        elif t == 'T': f[key] = struct.unpack_from('<q', aa, p)[0]; p += 12
        elif t == 'S': ln = struct.unpack_from('<H', aa, p)[0]; f[key] = aa[p + 2:p + 2 + ln]; p += 2 + ln
        elif t == 'B': ln = struct.unpack_from('<Q', aa, p)[0]; f[key] = ln; p += 8; blob_after += ln
        elif t == 'C': ln = struct.unpack_from('<I', aa, p)[0]; f[key] = ln; p += 4; blob_after += ln
        elif t == 'F': ln = struct.unpack_from('<I', aa, p)[0]; f[key] = ln; p += 4; blob_after += ln   # F = u32 size blob (after header)
        elif t == 'A': ln = struct.unpack_from('<H', aa, p)[0]; f[key] = aa[p + 2:p + 2 + ln]; p += 2 + ln
        elif t == 'H': f[key] = aa[p:p + 20].hex(); p += 20       # SHA-1 digest
        elif t == 'G': f[key] = aa[p:p + 32].hex(); p += 32      # SHA-256
        elif t == 'I': f[key] = aa[p:p + 48].hex(); p += 48      # SHA-384
        elif t == 'J': f[key] = aa[p:p + 64].hex(); p += 64      # SHA-512
        else: raise SystemExit(f'unknown field type {t!r} for {key} at {p:#x}')
    typ = chr(f.get('TYP', 0)) if isinstance(f.get('TYP'), int) else f.get('TYP')
    path = f.get('PAT', '')
    out.write(f"{path}\t{typ}\t{oct(f.get('MOD', 0))}\t{f.get('UID', 0)}\t{f.get('GID', 0)}\t{f.get('SIZ', f.get('DAT', 0))}\t{f.get('LNK', '')}\n")
    n += 1
    pos = end + blob_after
print(f'{n} entries -> {sys.argv[2]}', file=sys.stderr)
