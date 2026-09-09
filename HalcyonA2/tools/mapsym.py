"""Resolve HalcyonA2.dll RVAs (the "SELF +0x..." lines in %TEMP%\HalcyonA2.log) to symbols
using the linker map emitted by the Release build."""
import re, sys, io, os

MAP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   r"..\build\22284\x64\Release\HalcyonA2.map")

def load(mapfile=MAP):
    syms = []
    rx = re.compile(r"^\s*([0-9A-Fa-f]{4}):([0-9A-Fa-f]{8})\s+(\S+)\s+([0-9A-Fa-f]{16})\s")
    base = None
    for line in io.open(mapfile, encoding="utf-8", errors="replace"):
        m = re.match(r"^\s*Preferred load address is ([0-9A-Fa-f]+)", line)
        if m:
            base = int(m.group(1), 16)
        m = rx.match(line)
        if m:
            va = int(m.group(4), 16)
            syms.append((va, m.group(3)))
    if base is None:
        base = 0x180000000
    syms.sort()
    return base, syms

def resolve(rva, base, syms):
    va = base + rva
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= va:
            best = syms[mid]; lo = mid + 1
        else:
            hi = mid - 1
    if best is None:
        return "?"
    return "%s +0x%X" % (best[1], va - best[0])

if __name__ == "__main__":
    base, syms = load()
    args = sys.argv[1:]
    if not args:
        args = [l.strip() for l in sys.stdin if l.strip()]
    for a in args:
        for m in re.finditer(r"(?:SELF \+)?0x([0-9A-Fa-f]+)", a):
            r = int(m.group(1), 16)
            print("0x%-8X %s" % (r, resolve(r, base, syms)))
