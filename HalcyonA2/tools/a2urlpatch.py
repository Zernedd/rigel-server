#!/usr/bin/env python3
r"""
a2urlpatch.py - find and rewrite backend URLs in A2 / Orion Drift client data,
without changing the size of a single file.

Everything this tool writes is size-preserving:
  * the container (.obb / .pak / .so) keeps its exact byte length
  * every pak entry keeps its offset, so no other entry moves
  * replacement strings must be the same length as the originals

WHERE THE URLS ACTUALLY LIVE (build 20996)

  Mothership   pak entry  A2/Config/DefaultEngine.ini
                          [OnlineSubsystemMothership]
                          BaseUrl = "https://aa-mothership.com"
               ...inside an Oodle-compressed pak entry, so a plain byte search
               of the .obb finds nothing.

  Dashboard    NOT in the paks. It is a compile-time constant next to
               StationDashboardConstants.h in the game binary:
                 https://api.oriondrift.net
               Android: lib/arm64-v8a/libUnreal.so inside the APK
               PC:      A2-Win64-Shipping.exe
               Patch those with `patch-bin`.

HOW A COMPRESSED PAK ENTRY IS PATCHED IN PLACE

  Oodle has no free encoder, so the entry cannot be re-Oodled. Instead the entry
  is re-encoded with Zlib, which UE supports natively for pak entries:

    1. decompress every block (Oodle, via ooz)
    2. replace the string (same length, so the uncompressed size never changes)
    3. re-compress each block with zlib
    4. write the blocks back into the entry's own footprint plus the padding
       slack that follows it, before the next entry begins
    5. fix up, all fixed-width so nothing moves:
         - the entry header in the pak: Size, block ranges, method index, SHA1
         - the encoded entry in the pak index: size + block sizes + method index
         - the footer's compression-method table: add "Zlib"
         - the footer's index SHA1
         - the zip CRC32 of the pak inside the .obb (local + central header)

  Zlib is bigger than Oodle (~+6% here), which is why the trailing slack matters.
  The tool refuses if the re-encoded entry would not fit.

REQUIREMENTS
  Oodle decompression needs ooz.dll next to this script (tools/oodle/ooz.dll).
  Build it with tools/Build-Ooz.ps1. Uncompressed entries need no DLL.

EXAMPLES
  python a2urlpatch.py list   ..\..\main.33694970.com.AnotherAxiom.A2.obb
  python a2urlpatch.py find   ..\..\main.33694970.com.AnotherAxiom.A2.obb "aa-mothership.com"
  python a2urlpatch.py patch  ..\..\main.33694970.com.AnotherAxiom.A2.obb \
        --old "https://aa-mothership.com" --new "http://10.0.0.5:8080/aaa"
  python a2urlpatch.py patch  ...obb --old ... --new ... --write
  python a2urlpatch.py patch-bin libUnreal.so --old "https://api.oriondrift.net" \
        --new "http://192.168.1.9:9000/x" --write
  python a2urlpatch.py verify ..\..\main.33694970.com.AnotherAxiom.A2.obb
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import os
import struct
import sys
import zlib
from dataclasses import dataclass, field

PAK_MAGIC = 0x5A6F12E1
MAX_COMPRESSION_METHODS = 5
COMPRESSION_NAME_LEN = 32


# ---------------------------------------------------------------------------
# Oodle (decompression only, via the open-source ooz)
# ---------------------------------------------------------------------------

class Oodle:
    def __init__(self, dll_path: str | None = None):
        self.lib = None
        candidates = [dll_path] if dll_path else []
        here = os.path.dirname(os.path.abspath(__file__))
        candidates += [os.path.join(here, "oodle", "ooz.dll"), os.path.join(here, "ooz.dll")]
        for c in candidates:
            if c and os.path.isfile(c):
                self.lib = ctypes.CDLL(c)
                self.lib.OozDecompress.argtypes = [ctypes.c_char_p, ctypes.c_longlong,
                                                   ctypes.c_char_p, ctypes.c_longlong]
                self.lib.OozDecompress.restype = ctypes.c_int
                self.path = c
                break

    @property
    def available(self) -> bool:
        return self.lib is not None

    def decompress(self, src: bytes, out_size: int) -> bytes:
        if not self.lib:
            raise RuntimeError(
                "ooz.dll not found - needed to read Oodle-compressed pak entries.\n"
                "Build it with tools\\Build-Ooz.ps1, or pass --ooz <path to ooz.dll>.")
        buf = ctypes.create_string_buffer(out_size + 64)
        n = self.lib.OozDecompress(src, len(src), buf, out_size)
        if n != out_size:
            raise RuntimeError(f"Oodle decompress failed (returned {n}, wanted {out_size})")
        return buf.raw[:n]


# ---------------------------------------------------------------------------
# Container: a plain file, or one STORE'd entry inside an .obb (zip)
# ---------------------------------------------------------------------------

@dataclass
class Window:
    """A byte range inside a container file that holds one logical file."""
    path: str
    start: int          # absolute offset of the logical file's first byte
    size: int
    name: str = ""      # zip entry name, when it came from an .obb
    zip_local_crc_off: int = -1
    zip_central_crc_off: int = -1

    def read(self, off: int, n: int) -> bytes:
        with open(self.path, "rb") as f:
            f.seek(self.start + off)
            return f.read(n)

    def read_all(self) -> bytes:
        return self.read(0, self.size)

    def write(self, off: int, data: bytes) -> None:
        if off < 0 or off + len(data) > self.size:
            raise RuntimeError("refusing to write outside the file window "
                               f"(off={off} len={len(data)} size={self.size})")
        with open(self.path, "r+b") as f:
            f.seek(self.start + off)
            f.write(data)

    def fix_zip_crc(self) -> int | None:
        """Recompute the zip CRC32 for this entry (local + central header)."""
        if self.zip_local_crc_off < 0:
            return None
        crc = 0
        with open(self.path, "rb") as f:
            f.seek(self.start)
            left = self.size
            while left:
                chunk = f.read(min(1 << 22, left))
                if not chunk:
                    break
                crc = zlib.crc32(chunk, crc)
                left -= len(chunk)
        packed = struct.pack("<I", crc & 0xFFFFFFFF)
        with open(self.path, "r+b") as f:
            f.seek(self.zip_local_crc_off); f.write(packed)
            f.seek(self.zip_central_crc_off); f.write(packed)
        return crc & 0xFFFFFFFF


def open_container(path: str, want: str | None = None) -> Window:
    """Return a Window for `path`, or for the pak inside it if it is an .obb."""
    with open(path, "rb") as f:
        magic = f.read(4)
    if magic != b"PK\x03\x04":
        size = os.path.getsize(path)
        return Window(path=path, start=0, size=size, name=os.path.basename(path))

    import zipfile
    with zipfile.ZipFile(path) as z:
        infos = z.infolist()
        if want:
            cands = [i for i in infos if i.filename == want or i.filename.endswith("/" + want)]
        else:
            cands = [i for i in infos if i.filename.endswith(".pak")]
        if not cands:
            raise RuntimeError(f"no pak entry found in {path} (use --entry to name one)")
        if len(cands) > 1 and not want:
            names = ", ".join(i.filename for i in cands)
            raise RuntimeError(f"several pak entries ({names}); pick one with --entry")
        info = cands[0]
        if info.compress_type != 0:
            raise RuntimeError(f"{info.filename} is deflated inside the obb; "
                               "in-place patching needs a STORE'd entry")

    # Local header: sig(4) crc@14, then name/extra lengths decide where data starts.
    with open(path, "rb") as f:
        f.seek(info.header_offset)
        lh = f.read(30)
        if lh[:4] != b"PK\x03\x04":
            raise RuntimeError("bad local header")
        name_len, extra_len = struct.unpack_from("<HH", lh, 26)
        data_start = info.header_offset + 30 + name_len + extra_len
    central_crc = _find_central_crc_offset(path, info.filename)
    return Window(path=path, start=data_start, size=info.file_size, name=info.filename,
                  zip_local_crc_off=info.header_offset + 14,
                  zip_central_crc_off=central_crc)


def _find_central_crc_offset(path: str, name: str) -> int:
    """Offset of the CRC field in the central-directory record for `name`."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        f.seek(max(0, size - 66000))
        tail = f.read()
        eocd = tail.rfind(b"PK\x05\x06")
        if eocd < 0:
            raise RuntimeError("no zip EOCD found")
        cd_size, cd_off = struct.unpack_from("<II", tail, eocd + 12)
        f.seek(cd_off)
        cd = f.read(cd_size)
    o = 0
    target = name.encode()
    while o < len(cd) and cd[o:o + 4] == b"PK\x01\x02":
        nlen, elen, clen = struct.unpack_from("<HHH", cd, o + 28)
        entry_name = cd[o + 46:o + 46 + nlen]
        if entry_name == target:
            return cd_off + o + 16
        o += 46 + nlen + elen + clen
    raise RuntimeError(f"{name} not found in the zip central directory")


# ---------------------------------------------------------------------------
# Pak parsing
# ---------------------------------------------------------------------------

class Reader:
    def __init__(self, buf: bytes, off: int = 0):
        self.b, self.o = buf, off

    def i32(self):  v = struct.unpack_from("<i", self.b, self.o)[0]; self.o += 4; return v
    def u32(self):  v = struct.unpack_from("<I", self.b, self.o)[0]; self.o += 4; return v
    def i64(self):  v = struct.unpack_from("<q", self.b, self.o)[0]; self.o += 8; return v
    def u64(self):  v = struct.unpack_from("<Q", self.b, self.o)[0]; self.o += 8; return v
    def raw(self, n): v = self.b[self.o:self.o + n]; self.o += n; return v

    def fstring(self) -> str:
        n = self.i32()
        if n == 0:
            return ""
        if n < 0:
            return self.raw(-n * 2).decode("utf-16-le").rstrip("\0")
        return self.raw(n).decode("latin1").rstrip("\0")


@dataclass
class Entry:
    name: str
    encoded_off: int      # offset of this entry's record inside the primary index blob
    offset: int           # offset of the entry header in the pak
    size: int             # compressed size (payload only)
    usize: int            # uncompressed size
    cmi: int              # compression method index, 0 = stored
    encrypted: bool
    nblocks: int
    block_size: int
    blocks: list = field(default_factory=list)   # per-block compressed sizes


class Pak:
    def __init__(self, win: Window, oodle: Oodle):
        self.win, self.oodle = win, oodle
        self._read_footer()
        self._read_index()

    # ---- footer -----------------------------------------------------------
    def _read_footer(self):
        tail_len = min(4096, self.win.size)
        tail = self.win.read(self.win.size - tail_len, tail_len)
        base = self.win.size - tail_len
        hits = [base + i for i in range(len(tail) - 4)
                if struct.unpack_from("<I", tail, i)[0] == PAK_MAGIC]
        if not hits:
            raise RuntimeError("pak magic not found - is this a .pak?")
        self.magic_off = hits[-1]
        r = Reader(self.win.read(self.magic_off, tail_len), 0)
        r.u32()                                  # magic
        self.version = r.u32()
        self.index_offset = r.u64()
        self.index_size = r.u64()
        self.index_hash_off = self.magic_off + 24
        self.index_hash = r.raw(20)
        self.methods_off = self.magic_off + 44
        self.methods = []
        for i in range(MAX_COMPRESSION_METHODS):
            nm = r.raw(COMPRESSION_NAME_LEN).split(b"\0")[0].decode("latin1")
            self.methods.append(nm)
        self.encrypted_index = self.win.read(self.magic_off - 1, 1)[0] != 0
        if self.version < 8:
            raise RuntimeError(f"pak version {self.version} is not supported (need 8+)")
        if self.encrypted_index:
            raise RuntimeError("this pak has an encrypted index; not supported")

    def method_name(self, cmi: int) -> str:
        return "None" if cmi == 0 else (self.methods[cmi - 1] or f"<slot{cmi}>")

    # ---- index ------------------------------------------------------------
    def _read_index(self):
        self.index_blob = self.win.read(self.index_offset, self.index_size)
        r = Reader(self.index_blob)
        self.mount_point = r.fstring()
        self.num_entries = r.i32()
        r.u64()                                  # path hash seed
        if r.i32():
            r.i64(); r.i64(); r.raw(20)          # path hash index
        self.fdi_off = self.fdi_size = None
        if r.i32():
            self.fdi_off = r.i64(); self.fdi_size = r.i64(); r.raw(20)
        self.encoded_off_in_index = r.o + 4      # skip the size prefix
        enc_size = r.i32()
        self.encoded = r.raw(enc_size)

        if self.fdi_off is None:
            raise RuntimeError("pak has no full directory index; cannot resolve names")
        fdi = self.win.read(self.fdi_off, self.fdi_size)
        r2 = Reader(fdi)
        self.files: dict[str, int] = {}
        for _ in range(r2.i32()):
            d = r2.fstring()
            for _ in range(r2.i32()):
                fn = r2.fstring()
                self.files[d + fn] = r2.i32()

    # ---- encoded entry records -------------------------------------------
    @staticmethod
    def decode_entry(buf: bytes, o: int, name: str) -> tuple[Entry, int]:
        start = o
        v = struct.unpack_from("<I", buf, o)[0]; o += 4
        bs = (v & 0x3F) << 11
        if (v & 0x3F) == 0x3F:
            bs = struct.unpack_from("<I", buf, o)[0]; o += 4
        nblocks = (v >> 6) & 0xFFFF
        encrypted = bool((v >> 22) & 1)
        cmi = (v >> 23) & 0x3F
        off32, usz32, sz32 = (v >> 31) & 1, (v >> 30) & 1, (v >> 29) & 1

        def rd(is32):
            nonlocal o
            if is32:
                x = struct.unpack_from("<I", buf, o)[0]; o += 4
            else:
                x = struct.unpack_from("<Q", buf, o)[0]; o += 8
            return x

        offset = rd(off32)
        usize = rd(usz32)
        size = rd(sz32) if cmi != 0 else usize
        blocks = []
        if cmi != 0 and (nblocks > 1 or (nblocks == 1 and encrypted)):
            for _ in range(nblocks):
                blocks.append(struct.unpack_from("<I", buf, o)[0]); o += 4
        return Entry(name=name, encoded_off=start, offset=offset, size=size, usize=usize,
                     cmi=cmi, encrypted=encrypted, nblocks=nblocks, block_size=bs,
                     blocks=blocks), o

    def entry(self, name: str) -> Entry:
        e, _ = self.decode_entry(self.encoded, self.files[name], name)
        return e

    def entries(self):
        for name in sorted(self.files):
            yield self.entry(name)

    # ---- entry payload ----------------------------------------------------
    def read_header(self, e: Entry) -> dict:
        """The FPakEntry record serialized in the pak immediately before the data."""
        raw = self.win.read(e.offset, 128 + 16 * max(1, e.nblocks))
        r = Reader(raw)
        r.i64(); size = r.i64(); usize = r.i64()
        cmi = r.u32()
        hash_off_in_entry = r.o
        sha = r.raw(20)
        blocks = []
        if cmi != 0:
            n = r.i32()
            blocks_off = r.o
            for _ in range(n):
                s = r.i64(); en = r.i64()
                blocks.append((s, en))
        else:
            blocks_off = r.o
        flags = r.raw(1)[0]
        bs = r.u32()
        return dict(size=size, usize=usize, cmi=cmi, sha=sha, blocks=blocks, flags=flags,
                    block_size=bs, header_len=r.o,
                    sha_off=hash_off_in_entry, blocks_off=blocks_off,
                    size_off=8, cmi_off=24)

    def extract(self, e: Entry) -> bytes:
        h = self.read_header(e)
        if e.cmi == 0:
            return self.win.read(e.offset + h["header_len"], e.size)
        out = bytearray()
        for (s, en) in h["blocks"]:
            comp = self.win.read(e.offset + s, en - s)
            want = min(h["block_size"], e.usize - len(out))
            name = self.method_name(e.cmi).lower()
            if name.startswith("oodle") or name.startswith("kraken"):
                out += self.oodle.decompress(comp, want)
            elif name.startswith("zlib"):
                out += zlib.decompress(comp)
            else:
                raise RuntimeError(f"unsupported compression '{self.method_name(e.cmi)}'")
        return bytes(out)

    def entry_span(self, e: Entry) -> tuple[int, int]:
        """(first, last) absolute pak offsets used by this entry, header included."""
        h = self.read_header(e)
        if e.cmi == 0:
            return e.offset, e.offset + h["header_len"] + e.size
        return e.offset, e.offset + h["blocks"][-1][1]

    def slack_after(self, e: Entry) -> int:
        """Padding bytes between the end of this entry and whatever comes next."""
        _, end = self.entry_span(e)
        nxt = self.index_offset
        for other in self.entries():
            if other.offset > e.offset:
                nxt = min(nxt, other.offset)
        return nxt - end


# ---------------------------------------------------------------------------
# Patching
# ---------------------------------------------------------------------------

def replace_exact(blob: bytes, old: bytes, new: bytes) -> tuple[bytes, int]:
    if len(old) != len(new):
        raise RuntimeError(f"replacement must be the same length: "
                           f"old={len(old)} new={len(new)} (differs by {len(new)-len(old):+d})")
    count = blob.count(old)
    return blob.replace(old, new), count


def patch_pak_entry(pak: Pak, e: Entry, old: bytes, new: bytes, write: bool) -> bool:
    print(f"    entry   : {e.name}")
    print(f"    storage : {pak.method_name(e.cmi)}  size={e.size:,}  usize={e.usize:,}  blocks={e.nblocks}")

    blob = pak.extract(e)
    patched, count = replace_exact(blob, old, new)
    if count == 0:
        print("    nothing to do (string not present)")
        return False
    print(f"    replaced: {count} occurrence(s)")

    h = pak.read_header(e)

    # --- stored entries: straight overwrite -------------------------------
    if e.cmi == 0:
        data_off = e.offset + h["header_len"]
        print(f"    plan    : direct byte write at pak offset 0x{data_off:X} (no size change)")
        if not write:
            return True
        pak.win.write(data_off, patched)
        _fix_entry_hash(pak, e, h, patched)
        _fix_index_hash(pak)
        return True

    # --- compressed entries: re-encode with zlib into the same footprint ---
    bs = h["block_size"]
    chunks = [patched[i:i + bs] for i in range(0, len(patched), bs)] or [b""]
    if len(chunks) != len(h["blocks"]):
        raise RuntimeError(f"block count changed ({len(h['blocks'])} -> {len(chunks)}); "
                           "the uncompressed size must not change")
    zblocks = [zlib.compress(c, 9) for c in chunks]
    new_size = sum(len(z) for z in zblocks)

    first_block_start = h["blocks"][0][0]
    budget = e.size + pak.slack_after(e)
    print(f"    plan    : re-encode as Zlib  {e.size:,} -> {new_size:,} bytes "
          f"(budget {budget:,} = entry {e.size:,} + slack {budget - e.size:,})")
    if new_size > budget:
        raise RuntimeError(
            f"re-encoded entry does not fit: needs {new_size:,}, budget is {budget:,} "
            f"(short by {new_size - budget:,}).\n"
            "  Zlib is weaker than Oodle (~6% here) and this pak has no padding after the\n"
            "  entry to absorb the difference. A shorter replacement string does not help -\n"
            "  the length is fixed by the rule that nothing may move.\n"
            "  Options:\n"
            "    * override the value at runtime instead of patching: a Saved/Config\n"
            "      Engine.ini (or -ini: on the command line) beats the packaged Default,\n"
            "      costs nothing, and touches no game file.\n"
            "    * repack the pak with UnrealPak - changes file sizes.\n"
            "    * patch a build whose pak has slack: run `find` to see the slack per entry.")

    zlib_idx = _ensure_method(pak, "Zlib", write)

    # Lay the new blocks out from where the old ones started.
    ranges, cur = [], first_block_start
    payload = bytearray()
    for z in zblocks:
        ranges.append((cur, cur + len(z)))
        payload += z
        cur += len(z)

    if not write:
        print(f"    would write {len(payload):,} bytes at pak offset "
              f"0x{e.offset + first_block_start:X}; container size unchanged")
        return True

    pak.win.write(e.offset + first_block_start, bytes(payload))

    # entry header: Size, method index, block ranges, hash
    pak.win.write(e.offset + 8, struct.pack("<q", new_size))
    pak.win.write(e.offset + 24, struct.pack("<I", zlib_idx))
    blk = b"".join(struct.pack("<qq", s, en) for s, en in ranges)
    pak.win.write(e.offset + h["blocks_off"], blk)
    pak.win.write(e.offset + h["sha_off"], hashlib.sha1(bytes(payload)).digest())

    # index record: rewrite in place, same byte length
    _rewrite_encoded_entry(pak, e, new_size, zlib_idx, [len(z) for z in zblocks])
    _fix_index_hash(pak)
    return True


def _ensure_method(pak: Pak, name: str, write: bool) -> int:
    """Index (1-based) of `name` in the footer's compression table, adding it if needed."""
    for i, m in enumerate(pak.methods):
        if m.lower() == name.lower():
            return i + 1
    for i, m in enumerate(pak.methods):
        if not m:
            if write:
                pad = name.encode("latin1").ljust(COMPRESSION_NAME_LEN, b"\0")
                pak.win.write(pak.methods_off + i * COMPRESSION_NAME_LEN, pad)
                pak.methods[i] = name
            print(f"    methods : registering '{name}' in footer slot {i + 1} (fixed 32 bytes)")
            return i + 1
    raise RuntimeError("no free compression-method slot in the pak footer")


def _fix_entry_hash(pak: Pak, e: Entry, h: dict, payload: bytes) -> None:
    pak.win.write(e.offset + h["sha_off"], hashlib.sha1(payload).digest())


def _rewrite_encoded_entry(pak: Pak, e: Entry, new_size: int, cmi: int, block_sizes: list[int]) -> None:
    """Patch Size / method / block sizes inside the primary index, byte length unchanged."""
    buf = bytearray(pak.encoded)
    o = e.encoded_off
    v = struct.unpack_from("<I", buf, o)[0]
    v = (v & ~(0x3F << 23)) | ((cmi & 0x3F) << 23)
    struct.pack_into("<I", buf, o, v)
    o += 4
    if (v & 0x3F) == 0x3F:
        o += 4
    off32, usz32, sz32 = (v >> 31) & 1, (v >> 30) & 1, (v >> 29) & 1
    o += 4 if off32 else 8          # offset (unchanged)
    o += 4 if usz32 else 8          # uncompressed size (unchanged)
    if sz32:
        if new_size > 0xFFFFFFFF:
            raise RuntimeError("new size no longer fits the 32-bit field")
        struct.pack_into("<I", buf, o, new_size); o += 4
    else:
        struct.pack_into("<Q", buf, o, new_size); o += 8
    if e.blocks:                     # per-block sizes are only stored when >1 block
        if len(e.blocks) != len(block_sizes):
            raise RuntimeError("block count changed in the index record")
        for bsz in block_sizes:
            struct.pack_into("<I", buf, o, bsz); o += 4
    pak.encoded = bytes(buf)
    pak.win.write(pak.index_offset + pak.encoded_off_in_index + e.encoded_off,
                  pak.encoded[e.encoded_off:o])


def _fix_index_hash(pak: Pak) -> None:
    blob = pak.win.read(pak.index_offset, pak.index_size)
    pak.win.write(pak.index_hash_off, hashlib.sha1(blob).digest())


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_list(args):
    win = open_container(args.container, args.entry)
    pak = Pak(win, Oodle(args.ooz))
    print(f"{win.name}: pak v{pak.version}, {pak.num_entries} entries, "
          f"mount {pak.mount_point!r}, methods {[m for m in pak.methods if m]}")
    rows = []
    for e in pak.entries():
        if args.filter and args.filter.lower() not in e.name.lower():
            continue
        rows.append((e.name, e.offset, e.size, e.usize, pak.method_name(e.cmi)))
    for name, off, size, usize, m in rows[:args.limit]:
        print(f"  {name:<62} off=0x{off:<9X} size={size:<9,} usize={usize:<9,} {m}")
    print(f"  ({len(rows)} shown{'' if len(rows) <= args.limit else f', limited to {args.limit}'})")


def _search_entries(pak: Pak, needle: bytes, only: str | None):
    for e in pak.entries():
        if only and only.lower() not in e.name.lower():
            continue
        # cheap gate: only decompress things that could hold text
        if e.usize > 8 * 1024 * 1024:
            continue
        try:
            blob = pak.extract(e)
        except Exception:
            continue
        if needle in blob:
            yield e, blob


def cmd_find(args):
    win = open_container(args.container, args.entry)
    pak = Pak(win, Oodle(args.ooz))
    needle = args.text.encode()
    print(f"searching {win.name} for {args.text!r} ...")
    hits = 0
    for e, blob in _search_entries(pak, needle, args.only):
        hits += 1
        n = blob.count(needle)
        print(f"\n  {e.name}  ({n} hit{'s' if n > 1 else ''}, {pak.method_name(e.cmi)}, "
              f"slack after entry: {pak.slack_after(e):,} bytes)")
        i = blob.find(needle)
        line_start = blob.rfind(b"\n", 0, i) + 1
        line_end = blob.find(b"\n", i)
        line = blob[line_start:line_end if line_end > 0 else i + 80]
        print(f"      {line.decode('utf-8', 'replace').strip()[:160]}")
    if not hits:
        print("  not found in any pak entry.")
        print("  Remember: the Dashboard URL is compiled into the game binary, not the paks -")
        print("  search libUnreal.so (Android) or A2-Win64-Shipping.exe (PC) with `find-bin`.")


def cmd_find_bin(args):
    data = open(args.binary, "rb").read()
    needle = args.text.encode()
    offs = []
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        offs.append(i); start = i + 1
    print(f"{args.binary}: {len(offs)} occurrence(s) of {args.text!r}")
    for o in offs[:20]:
        print(f"  file offset 0x{o:X}")


def cmd_patch(args):
    win = open_container(args.container, args.entry)
    pak = Pak(win, Oodle(args.ooz))
    old, new = args.old.encode(), args.new.encode()
    replace_exact(b"", b"", b"")  # no-op, keeps the length rule in one place
    if len(old) != len(new):
        raise SystemExit(f"error: replacement must be the same length "
                         f"({len(old)} vs {len(new)}, differs by {len(new)-len(old):+d}).\n"
                         f"       pad the new URL to exactly {len(old)} characters.")
    size_before = os.path.getsize(win.path)
    print(f"container : {win.path}")
    print(f"pak entry : {win.name}  ({win.size:,} bytes at 0x{win.start:X})")
    print(f"old       : {args.old}  ({len(old)} chars)")
    print(f"new       : {args.new}  ({len(new)} chars)")
    print("mode      : " + ("WRITE" if args.write else "dry run (pass --write to apply)"))

    touched = 0
    for e, _ in _search_entries(pak, old, args.only):
        print()
        if patch_pak_entry(pak, e, old, new, args.write):
            touched += 1
    if touched == 0:
        raise SystemExit("error: string not found in any pak entry")

    if args.write:
        crc = win.fix_zip_crc()
        if crc is not None:
            print(f"\n  zip CRC32 for {win.name} updated -> 0x{crc:08X}")
        size_after = os.path.getsize(win.path)
        print(f"  container size: {size_before:,} -> {size_after:,} "
              f"({'unchanged' if size_before == size_after else 'CHANGED - THIS IS A BUG'})")
        if size_before != size_after:
            raise SystemExit("error: container size changed; restore from backup")
    print(f"\n{touched} entr{'y' if touched == 1 else 'ies'} "
          f"{'patched' if args.write else 'would be patched'}.")


def cmd_patch_bin(args):
    old, new = args.old.encode(), args.new.encode()
    if len(old) != len(new):
        raise SystemExit(f"error: replacement must be the same length "
                         f"({len(old)} vs {len(new)}, differs by {len(new)-len(old):+d})")
    size_before = os.path.getsize(args.binary)
    data = open(args.binary, "rb").read()
    n = data.count(old)
    print(f"{args.binary}: {n} occurrence(s) of {args.old!r}")
    if n == 0:
        raise SystemExit("error: string not found")
    if not args.write:
        print("dry run - pass --write to apply")
        return
    start = 0
    with open(args.binary, "r+b") as f:
        while True:
            i = data.find(old, start)
            if i < 0:
                break
            f.seek(i); f.write(new)
            print(f"  patched at 0x{i:X}")
            start = i + 1
    size_after = os.path.getsize(args.binary)
    print(f"size: {size_before:,} -> {size_after:,} "
          f"({'unchanged' if size_before == size_after else 'CHANGED - THIS IS A BUG'})")


def cmd_verify(args):
    win = open_container(args.container, args.entry)
    pak = Pak(win, Oodle(args.ooz))
    blob = win.read(pak.index_offset, pak.index_size)
    ok_index = hashlib.sha1(blob).digest() == pak.index_hash
    print(f"pak index hash : {'OK' if ok_index else 'MISMATCH'}")
    bad = 0
    checked = 0
    for e in pak.entries():
        h = pak.read_header(e)
        if e.cmi == 0:
            payload = win.read(e.offset + h["header_len"], e.size)
        else:
            payload = win.read(e.offset + h["blocks"][0][0], e.size)
        checked += 1
        if hashlib.sha1(payload).digest() != h["sha"]:
            bad += 1
            if bad <= 5:
                print(f"  entry hash mismatch: {e.name}")
    print(f"entry hashes   : {checked - bad}/{checked} OK")
    if win.zip_local_crc_off >= 0:
        crc = 0
        with open(win.path, "rb") as f:
            f.seek(win.start); left = win.size
            while left:
                c = f.read(min(1 << 22, left))
                crc = zlib.crc32(c, crc); left -= len(c)
        with open(win.path, "rb") as f:
            f.seek(win.zip_local_crc_off); stored = struct.unpack("<I", f.read(4))[0]
        print(f"obb zip CRC32  : {'OK' if stored == (crc & 0xFFFFFFFF) else 'MISMATCH'} "
              f"(stored 0x{stored:08X}, actual 0x{crc & 0xFFFFFFFF:08X})")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[1],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ooz", help="path to ooz.dll (default: tools/oodle/ooz.dll)")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_container(sp):
        sp.add_argument("container", help=".obb or .pak")
        sp.add_argument("--entry", help="pak entry inside the .obb, if there are several")

    sp = sub.add_parser("list", help="list pak entries")
    add_container(sp)
    sp.add_argument("--filter", help="substring of the file name")
    sp.add_argument("--limit", type=int, default=60)
    sp.set_defaults(func=cmd_list)

    sp = sub.add_parser("find", help="find a string inside pak entries")
    add_container(sp)
    sp.add_argument("text")
    sp.add_argument("--only", help="restrict to entries whose name contains this")
    sp.set_defaults(func=cmd_find)

    sp = sub.add_parser("find-bin", help="find a string in a plain binary (.so/.exe)")
    sp.add_argument("binary")
    sp.add_argument("text")
    sp.set_defaults(func=cmd_find_bin)

    sp = sub.add_parser("patch", help="replace a same-length string inside a pak")
    add_container(sp)
    sp.add_argument("--old", required=True)
    sp.add_argument("--new", required=True)
    sp.add_argument("--only", help="restrict to entries whose name contains this")
    sp.add_argument("--write", action="store_true", help="actually modify the file")
    sp.set_defaults(func=cmd_patch)

    sp = sub.add_parser("patch-bin", help="replace a same-length string in a plain binary")
    sp.add_argument("binary")
    sp.add_argument("--old", required=True)
    sp.add_argument("--new", required=True)
    sp.add_argument("--write", action="store_true")
    sp.set_defaults(func=cmd_patch_bin)

    sp = sub.add_parser("verify", help="re-check pak hashes and the obb CRC")
    add_container(sp)
    sp.set_defaults(func=cmd_verify)

    args = p.parse_args(argv)
    try:
        args.func(args)
    except RuntimeError as e:
        raise SystemExit(f"error: {e}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
