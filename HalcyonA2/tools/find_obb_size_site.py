#!/usr/bin/env python3
r"""
find_obb_size_site.py - locate the OBB-size check inside an A2 / Orion Drift APK.

UE writes the expansion file's length into a generated OBBData class, and the app refuses an OBB
whose length differs. R8 renames that class, so the "smali file" holding it has a DIFFERENT NAME IN
EVERY BUILD - which is why it has to be found rather than hardcoded.

This finds it properly: it parses classes*.dex, walks every method's bytecode, and reports each
`const-wide/32` (opcode 0x17) literal that equals the OBB size, together with the class and method
that contains it. That is the exact site to patch, and the value to patch it to.

    python find_obb_size_site.py --apk in.apk --obb in.obb
    python find_obb_size_site.py --apk in.apk --size 916362487
    python find_obb_size_site.py --apk in.apk --obb in.obb --dump-dir "..\..\nov client quest"

With --dump-dir the dex files are written out alongside a report, so the site can be inspected by
hand (baksmali, jadx, a hex editor) if wanted.
"""

from __future__ import annotations

import argparse
import os
import struct
import zipfile


# ------------------------------------------------------------------ minimal dex reader
def uleb128(buf: bytes, off: int) -> tuple[int, int]:
    result = 0
    shift = 0
    while True:
        b = buf[off]
        off += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, off
        shift += 7


class Dex:
    def __init__(self, data: bytes, name: str):
        self.d = data
        self.name = name
        (self.string_ids_size, self.string_ids_off) = struct.unpack_from("<II", data, 56)
        (self.type_ids_size, self.type_ids_off) = struct.unpack_from("<II", data, 64)
        (self.proto_ids_size, self.proto_ids_off) = struct.unpack_from("<II", data, 72)
        (self.field_ids_size, self.field_ids_off) = struct.unpack_from("<II", data, 80)
        (self.method_ids_size, self.method_ids_off) = struct.unpack_from("<II", data, 88)
        (self.class_defs_size, self.class_defs_off) = struct.unpack_from("<II", data, 96)

    def string(self, idx: int) -> str:
        off = struct.unpack_from("<I", self.d, self.string_ids_off + idx * 4)[0]
        _len, off = uleb128(self.d, off)
        end = self.d.index(b"\x00", off)
        return self.d[off:end].decode("utf-8", "replace")

    def type_str(self, idx: int) -> str:
        sidx = struct.unpack_from("<I", self.d, self.type_ids_off + idx * 4)[0]
        return self.string(sidx)

    def method_name(self, idx: int) -> str:
        _cls, _proto, name_idx = struct.unpack_from("<HHI", self.d, self.method_ids_off + idx * 8)
        return self.string(name_idx)

    def methods_with_code(self):
        """Yield (class_descriptor, method_name, insns_off, insns_bytes)."""
        for i in range(self.class_defs_size):
            base = self.class_defs_off + i * 32
            class_idx, _flags, _super, _ifaces, _src, _ann, data_off, _static = \
                struct.unpack_from("<IIIIIIII", self.d, base)
            if not data_off:
                continue
            cls = self.type_str(class_idx)
            off = data_off
            sf, off = uleb128(self.d, off)
            inf, off = uleb128(self.d, off)
            dm, off = uleb128(self.d, off)
            vm, off = uleb128(self.d, off)
            for _ in range(sf):
                _, off = uleb128(self.d, off)
                _, off = uleb128(self.d, off)
            for _ in range(inf):
                _, off = uleb128(self.d, off)
                _, off = uleb128(self.d, off)
            midx = 0
            for group, count in (("direct", dm), ("virtual", vm)):
                midx = 0
                for _ in range(count):
                    diff, off = uleb128(self.d, off)
                    _acc, off = uleb128(self.d, off)
                    code_off, off = uleb128(self.d, off)
                    midx += diff
                    if code_off:
                        insns_size = struct.unpack_from("<I", self.d, code_off + 12)[0]
                        ins_off = code_off + 16
                        yield cls, self.method_name(midx), ins_off, self.d[ins_off:ins_off + insns_size * 2]


def find_sites(dex: Dex, size: int):
    """Every const-wide/32 (0x17) whose literal equals `size`."""
    lit = struct.pack("<i", size)
    hits = []
    for cls, meth, ins_off, insns in dex.methods_with_code():
        start = 0
        while True:
            j = insns.find(lit, start)
            if j < 0:
                break
            # const-wide/32 is: opcode 0x17, vAA, then the 4-byte literal
            if j >= 2 and insns[j - 2] == 0x17:
                hits.append((cls, meth, ins_off + j, insns[j - 1]))
            start = j + 1
    return hits


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Find the OBB-size literal inside an APK's dex.")
    ap.add_argument("--apk", required=True)
    ap.add_argument("--obb", help="OBB whose size to look for")
    ap.add_argument("--size", type=int, help="size in bytes (instead of --obb)")
    ap.add_argument("--dump-dir", help="also write the dex files and a report here")
    a = ap.parse_args(argv)

    if a.obb:
        size = os.path.getsize(a.obb)
    elif a.size is not None:
        size = a.size
    else:
        ap.error("pass --obb or --size")

    print(f"APK        : {a.apk}")
    if a.obb:
        print(f"OBB        : {a.obb}")
    print(f"OBB size   : {size:,} bytes = 0x{size:X}")
    print()

    report = [f"APK      : {a.apk}",
              f"OBB size : {size} (0x{size:X})", ""]

    with zipfile.ZipFile(a.apk) as z:
        dexes = sorted(n for n in z.namelist() if n.endswith(".dex"))
        if a.dump_dir:
            os.makedirs(a.dump_dir, exist_ok=True)
        total = 0
        for n in dexes:
            data = z.read(n)
            if a.dump_dir:
                with open(os.path.join(a.dump_dir, os.path.basename(n)), "wb") as f:
                    f.write(data)
            dex = Dex(data, n)
            hits = find_sites(dex, size)
            line = f"{n}: {len(data):,} bytes, {dex.class_defs_size} classes, {len(hits)} size literal(s)"
            print(line)
            report.append(line)
            for cls, meth, off, reg in hits:
                total += 1
                # Lcom/foo/a$a; -> smali path com/foo/a$a.smali
                smali = cls[1:-1] + ".smali" if cls.startswith("L") and cls.endswith(";") else cls
                det = (f"    class  {cls}\n"
                       f"    smali  {smali}\n"
                       f"    method {meth}\n"
                       f"    at     file offset 0x{off:X} in {n}  (const-wide/32 v{reg}, #0x{size:X})")
                print(det)
                report.append(det)
        print()
        if total == 0:
            msg = ("No const-wide/32 literal matches that size.\n"
                   "  - is this the OBB that actually shipped with this APK?\n"
                   "  - a >2 GiB OBB cannot use this 32-bit form, and would be const-wide instead")
            print(msg)
            report.append(msg)
        else:
            msg = (f"{total} site(s). That is the per-version 'smali file' - patch the literal to the new\n"
                   f"OBB's size, then fix the dex checksum + SHA-1 signature, or flip\n"
                   f"bVerifyOBBOnStartUp in the manifest to skip the check entirely.\n"
                   f"build_client.py does both (patch_dex_obb_size / set_verify_obb_flag).")
            print(msg)
            report.append(msg)

    if a.dump_dir:
        rp = os.path.join(a.dump_dir, "obb-size-site.txt")
        with open(rp, "w", encoding="utf-8") as f:
            f.write("\n".join(report) + "\n")
        print(f"\nwrote {rp}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
