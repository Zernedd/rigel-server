#!/usr/bin/env python3
r"""
align_pairs.py - map data globals and callees between two A2 builds by aligning the
disassembly of functions already known to correspond.

For each (old_rva, new_rva) pair the two functions are disassembled and walked in step;
while the instruction streams agree (same mnemonic, same operand shape):
  * every RIP-relative memory operand yields   old_data_rva -> new_data_rva
  * every direct call yields                    old_callee   -> new_callee
Collected over all pairs that gives the new addresses of globals like GIsServer or a log
category verbosity byte (things with no signature of their own), and of functions that
only have generic prologues (SimIntegrate, found through the already-ported StepSim).

It also reports a similarity score per pair, which is how a function with several
signature candidates (NetVarReg) is disambiguated.

    python align_pairs.py OLD.exe NEW.exe pairs.json wanted.json [wanted-calls.json]
"""
import json, sys, os
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_OP_REG, X86_REG_RIP
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from port_offsets import PE

md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
NL = chr(10)


def shape(insn):
    """Operand shape ignoring build-volatile values."""
    parts = [insn.mnemonic]
    for op in insn.operands:
        if op.type == X86_OP_REG:
            parts.append("r%d" % op.reg)
        elif op.type == X86_OP_IMM:
            parts.append("imm" if insn.mnemonic.startswith(("call", "j")) else "imm%d" % (op.imm if -256 < op.imm < 256 else 0))
        elif op.type == X86_OP_MEM:
            parts.append("rip" if op.mem.base == X86_REG_RIP else "mem%d/%d" % (op.mem.base, op.mem.disp if abs(op.mem.disp) < 0x100000 else 0))
    return tuple(parts)


def disasm(pe, rva, size):
    off = pe.rva2off(rva)
    return list(md.disasm(pe.data[off:off + size], pe.imagebase + rva))


def align(old, new, o_rva, n_rva, o_size, n_size):
    A, B = disasm(old, o_rva, o_size), disasm(new, n_rva, n_size)
    i = j = 0; same = 0; maps = {}; calls = {}
    while i < len(A) and j < len(B):
        a, b = A[i], B[j]
        if shape(a) == shape(b):
            same += 1
            for oa, ob in zip(a.operands, b.operands):
                if oa.type == X86_OP_MEM and oa.mem.base == X86_REG_RIP and ob.type == X86_OP_MEM and ob.mem.base == X86_REG_RIP:
                    to = a.address + a.size + oa.mem.disp - old.imagebase
                    tn = b.address + b.size + ob.mem.disp - new.imagebase
                    maps.setdefault(to, {}).setdefault(tn, 0); maps[to][tn] += 1
                elif a.mnemonic == "call" and oa.type == X86_OP_IMM and ob.type == X86_OP_IMM:
                    to, tn = oa.imm - old.imagebase, ob.imm - new.imagebase
                    calls.setdefault(to, {}).setdefault(tn, 0); calls[to][tn] += 1
            i += 1; j += 1
        else:
            # resynchronise: skip whichever side has an inserted instruction
            if i + 1 < len(A) and shape(A[i + 1]) == shape(b): i += 1
            elif j + 1 < len(B) and shape(B[j + 1]) == shape(a): j += 1
            else: i += 1; j += 1
    score = same / max(1, max(len(A), len(B)))
    return score, maps, len(A), len(B), calls


def best_of(m):
    return sorted(m.items(), key=lambda kv: -kv[1])


def main():
    old, new = PE(sys.argv[1]), PE(sys.argv[2])
    pairs = json.load(open(sys.argv[3]))
    wanted = {int(v, 16): k for k, v in json.load(open(sys.argv[4])).items()}
    want_calls = {int(v, 16): k for k, v in json.load(open(sys.argv[5])).items()} if len(sys.argv) > 5 else {}
    allmaps, allcalls = {}, {}
    print(f"{'pair':<22} {'old':>10} {'new':>10}  score  insns  rip-maps")
    for p in pairs:
        o, n = int(p["old"], 16), int(p["new"], 16)
        score, maps, la, lb, calls = align(old, new, o, n, int(p.get("old_size", "0x200"), 16), int(p.get("new_size", "0x200"), 16))
        print(f"{p['name']:<22} {o:>#10x} {n:>#10x}  {score:5.2f}  {la:>3}/{lb:<3}  {len(maps)}")
        for k, v in maps.items():
            for nk, c in v.items():
                allmaps.setdefault(k, {}).setdefault(nk, 0); allmaps[k][nk] += c
        for k, v in calls.items():
            for nk, c in v.items():
                allcalls.setdefault(k, {}).setdefault(nk, 0); allcalls[k][nk] += c

    print(NL + "=== wanted globals ===")
    for orva, name in sorted(wanted.items()):
        m = allmaps.get(orva)
        if not m:
            print(f"  {name:<26} {orva:#x} -> (no reference in any aligned pair)"); continue
        b = best_of(m)
        print(f"  {name:<26} {orva:#x} -> {b[0][0]:#x}  (seen {b[0][1]}x" + (f"; also {[hex(k) for k, _ in b[1:]]}" if len(b) > 1 else "") + ")")

    print(NL + "=== mapped .data references near wanted globals (old -> new) ===")
    for orva in sorted(allmaps):
        for wrva, wname in wanted.items():
            if abs(orva - wrva) <= 0x40 and orva != wrva:
                b = best_of(allmaps[orva])[0]
                print(f"  {orva:#x} -> {b[0]:#x}   (near {wname}: old delta {orva - wrva:+#x})")

    if want_calls:
        print(NL + "=== wanted functions (via call targets in aligned pairs) ===")
        for orva, name in want_calls.items():
            m = allcalls.get(orva)
            if not m:
                print(f"  {name:<26} {orva:#x} -> (not called from any aligned pair)"); continue
            b = best_of(m)
            print(f"  {name:<26} {orva:#x} -> {b[0][0]:#x}  (seen {b[0][1]}x" + (f"; also {[hex(k) for k, _ in b[1:]]}" if len(b) > 1 else "") + ")")

    json.dump({hex(k): {hex(nk): c for nk, c in v.items()} for k, v in allmaps.items()}, open("align-maps.json", "w"), indent=1)
    json.dump({hex(k): {hex(nk): c for nk, c in v.items()} for k, v in allcalls.items()}, open("align-calls.json", "w"), indent=1)


if __name__ == "__main__":
    main()
