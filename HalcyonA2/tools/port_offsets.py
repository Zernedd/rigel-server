#!/usr/bin/env python3
r"""
port_offsets.py - first-pass port of code addresses from one A2 build to another.

For every old RVA it disassembles the old binary (capstone), builds a byte signature
with the build-volatile parts wildcarded (RIP-relative displacements, call/jmp/jcc
targets, 64-bit immediates), then searches the new binary's .text for it. A unique
hit is a candidate; ambiguous hits get a longer signature; misses fall back to a
looser signature that also wildcards struct displacements and 32-bit immediates.

Candidates are only that. The final word comes from IDA on the new binary: function
bounds, string xrefs and decompilation (see port-nov15.md). This script's job is to
make that verification a lookup rather than a search.

    python port_offsets.py OLD.exe NEW.exe sites.json  [-o result.json]

sites.json: [{"name": "...", "rva": "0x53FD500", "kind": "func|site|data", "note": "..."}]
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP


class PE:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        self.imagebase = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
        secoff = pe + 24 + optsz
        self.secs = []
        for i in range(nsec):
            o = secoff + i * 40
            name = d[o:o + 8].rstrip(b"\0").decode("latin1")
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, o + 8)
            self.secs.append((name, vaddr, vsize, rawptr, rawsize))
        self.text = next(s for s in self.secs if s[0] == ".text")

    def rva2off(self, rva):
        for n, va, vs, rp, rs in self.secs:
            if va <= rva < va + max(vs, rs):
                return rp + (rva - va)
        return None

    def off2rva(self, off):
        for n, va, vs, rp, rs in self.secs:
            if rp <= off < rp + rs:
                return va + (off - rp)
        return None

    def text_bytes(self):
        _, va, vs, rp, rs = self.text
        return self.data[rp:rp + rs], va


md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True


def build_sig(pe: PE, rva: int, min_fixed: int, max_len: int, loose: bool):
    """Return (pattern_bytes, mask) where mask[i] is True for a fixed byte."""
    off = pe.rva2off(rva)
    code = pe.data[off:off + max_len + 16]
    pat, mask = bytearray(), []
    fixed = 0
    for insn in md.disasm(code, pe.imagebase + rva):
        if len(pat) + insn.size > max_len:
            break
        b = bytearray(insn.bytes)
        m = [True] * insn.size
        enc = insn.encoding
        wild = []
        # RIP-relative displacement is always volatile between builds.
        for op in insn.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP and enc.disp_offset:
                wild.append((enc.disp_offset, enc.disp_size))
            elif loose and op.type == X86_OP_MEM and enc.disp_offset and enc.disp_size == 4:
                wild.append((enc.disp_offset, enc.disp_size))
        # Branch targets: call/jmp/jcc with rel32/rel8 immediates; 64-bit immediates.
        if insn.mnemonic.startswith(("call", "jmp", "j")) and enc.imm_offset:
            wild.append((enc.imm_offset, enc.imm_size))
        elif enc.imm_offset and enc.imm_size == 8:
            wild.append((enc.imm_offset, enc.imm_size))
        elif loose and enc.imm_offset and enc.imm_size == 4:
            wild.append((enc.imm_offset, enc.imm_size))
        for o, n in wild:
            for i in range(o, o + n):
                if i < insn.size:
                    m[i] = False
        pat += b
        mask += m
        fixed += sum(m)
        if fixed >= min_fixed and insn.mnemonic in ("ret", "int3", "jmp"):
            break
        if fixed >= min_fixed and len(pat) >= max_len // 2:
            break
    return bytes(pat), mask


def sig_to_regex(pat, mask):
    out = b""
    run = b""
    for byte, fixed in zip(pat, mask):
        if fixed:
            run += bytes([byte])
        else:
            if run:
                out += re.escape(run); run = b""
            out += b"."
    if run:
        out += re.escape(run)
    return re.compile(out, re.DOTALL)


def sig_text(pat, mask):
    return " ".join(f"{b:02X}" if f else "??" for b, f in zip(pat, mask))


def search(new: PE, pat, mask, cap=8):
    text, base = new.text_bytes()
    rx = sig_to_regex(pat, mask)
    hits = []
    for m in rx.finditer(text):
        hits.append(base + m.start())
        if len(hits) >= cap:
            break
    return hits


def port_one(old: PE, new: PE, rva: int):
    """Try progressively longer/looser signatures until exactly one hit."""
    attempts = []
    for loose in (False, True):
        for min_fixed, max_len in ((24, 64), (40, 96), (64, 160), (96, 256), (140, 400)):
            pat, mask = build_sig(old, rva, min_fixed, max_len, loose)
            hits = search(new, pat, mask)
            attempts.append((loose, len(pat), len(hits)))
            if len(hits) == 1:
                return dict(new_rva=hits[0], hits=1, sig_len=len(pat), loose=loose,
                            sig=sig_text(pat, mask), attempts=attempts)
            if len(hits) == 0 and not loose:
                break   # longer strict sigs will not help; go loose
    pat, mask = build_sig(old, rva, 40, 96, True)
    hits = search(new, pat, mask)
    return dict(new_rva=None, hits=len(hits), candidates=[hex(h) for h in hits],
                sig_len=len(pat), loose=True, sig=sig_text(pat, mask), attempts=attempts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("old"); ap.add_argument("new"); ap.add_argument("sites")
    ap.add_argument("-o", "--out")
    a = ap.parse_args()
    old, new = PE(a.old), PE(a.new)
    sites = json.load(open(a.sites))
    results = []
    print(f"{'name':<22} {'old':>10}  {'new':>10}  hits  sig  note")
    for s in sites:
        rva = int(s["rva"], 16)
        if s.get("kind") == "data":
            results.append(dict(s, status="data (resolve via IDA xrefs)"))
            print(f"{s['name']:<22} {rva:>#10x}  {'-':>10}     -    -  data - resolve via IDA")
            continue
        r = port_one(old, new, rva)
        r.update(name=s["name"], old_rva=rva, kind=s.get("kind", "func"), note=s.get("note", ""))
        results.append(r)
        nr = f"{r['new_rva']:#x}" if r["new_rva"] else "?"
        delta = f"{r['new_rva'] - rva:+#x}" if r["new_rva"] else ""
        flag = "" if r["new_rva"] else f"  <-- {r['hits']} hits {r.get('candidates', '')}"
        print(f"{s['name']:<22} {rva:>#10x}  {nr:>10}  {r['hits']:>4}  {r['sig_len']:>3}  "
              f"{'loose' if r['loose'] else 'strict'} {delta}{flag}")
    if a.out:
        json.dump(results, open(a.out, "w"), indent=2,
                  default=lambda x: hex(x) if isinstance(x, int) else str(x))
        print(f"\nwritten {a.out}")


if __name__ == "__main__":
    main()
