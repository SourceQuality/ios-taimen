#!/usr/bin/env python3
"""Decode the command stream a GPU hang left in the log (xnu 0376's "SWEET GPU: hang IB1 ..." lines).

  pm4dump.py LOG [MESA_SRC]     MESA_SRC: a Mesa tree for the names (default ../research/m0/mesa)

For IB1 and IB2: the dwords around where the CP stood, decoded as a5xx packets (type 4: register writes, named
from a5xx.xml; type 7: opcodes from adreno_pm4.xml), from the first offset whose headers chain with valid
parity; ">>" marks the packet holding the CP's place (an IB's start plus its size less the dwords left, xnu
0377; its start where the size is unknown). Only the last hang's lines are used.
"""
import os
import re
import sys


def parity(v):
    """Mesa's pm4_odd_parity_bit: the bit that makes the value's ones odd (its nibbles XORed, then a table)."""
    x = v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12) ^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)
    return (0x9669 >> (x & 0xf)) & 1


def names(mesa):
    ops, regs = {}, {}
    pm4 = os.path.join(mesa, 'src/freedreno/registers/adreno/adreno_pm4.xml')
    a5 = os.path.join(mesa, 'src/freedreno/registers/adreno/a5xx.xml')
    if os.path.exists(pm4):
        x = open(pm4).read()
        m = re.search(r'<enum name="adreno_pm4_type3_packets"[^>]*>(.*?)</enum>', x, re.S)
        for v in re.finditer(r'<value name="([A-Z0-9_]+)" value="(0x[0-9a-fA-F]+|\d+)"', m[1] if m else ''):
            ops.setdefault(int(v[2], 0), v[1])
    if os.path.exists(a5):
        for m in re.finditer(r'<reg(?:32|64) offset="(0x[0-9a-fA-F]+)" name="([A-Za-z0-9_]+)"', open(a5).read()):
            regs.setdefault(int(m[1], 16), m[2])
    return ops, regs


def packet(w):
    """(kind, length in dwords including the header, label) or None."""
    t = w >> 28
    if t == 4:
        cnt, reg = w & 0x7f, (w >> 8) & 0x7ffff
        if (w >> 7) & 1 != parity(cnt) or (w >> 27) & 1 != parity(reg):
            return None
        return 4, 1 + cnt, reg
    if t == 7:
        cnt, op = w & 0x7fff, (w >> 16) & 0x7f
        if (w >> 15) & 1 != parity(cnt) or (w >> 23) & 1 != parity(op):
            return None
        return 7, 1 + cnt, op
    return None


def chain(words, start):
    n, i = 0, start
    while i < len(words):
        p = packet(words[i])
        if p is None:
            break
        i += p[1]
        n += 1
    return n


def main():
    log = sys.argv[1]
    mesa = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../research/m0/mesa')
    ops, regs = names(mesa)
    ibs, heads = {}, {}
    for line in open(log, encoding='utf-8', errors='replace'):
        m = re.search(r'SWEET GPU: hang (IB[12]) at ([0-9a-f]+), (\d+) dwords left(?: of (\d+))?', line)
        if m:
            if m[1] == 'IB1':
                ibs, heads = {}, {}
            heads[m[1]] = (int(m[2], 16), int(m[3]), int(m[4]) if m[4] else 0)
            ibs[m[1]] = []
            continue
        m = re.search(r'SWEET GPU: hang (IB[12]) ([0-9a-f]+)([:*]) ((?:[0-9a-f]{8} ?){8})', line)
        if m and m[1] in ibs:
            ibs[m[1]].append((int(m[2], 16), [int(x, 16) for x in m[4].split()]))
    for ib in ('IB1', 'IB2'):
        if ib not in heads:
            continue
        base, left, size = heads[ib]
        rows = ibs.get(ib, [])
        at = base + 4 * (size - left) if size >= left else base
        print(f'== {ib}: starts at {base:#x}, {size or "?"} dwords, {left} left: the CP at {at:#x}')
        if not rows:
            print('   (no dwords)')
            continue
        start_va = rows[0][0]
        words = [w for _, ws in rows for w in ws]
        cp = (at - start_va) // 4
        best = max(range(min(64, len(words))), key=lambda s: chain(words, s))
        i = best
        while i < len(words):
            p = packet(words[i])
            if p is None:
                print(f'   {start_va + 4 * i:#x}: {words[i]:08x}  (not a packet: stop)')
                break
            kind, length, what = p
            mark = '>>' if i <= cp < i + length else '  '
            body = ' '.join(f'{x:08x}' for x in words[i + 1:i + min(length, 9)])
            more = ' ...' if length > 9 else ''
            if kind == 4:
                label = f'pkt4 {regs.get(what, hex(what))} x{length - 1}'
            else:
                label = f'pkt7 {ops.get(what, hex(what))} x{length - 1}'
            print(f'{mark} {start_va + 4 * i:#x}: {label:44} {body}{more}')
            i += length


main()
