#!/usr/bin/env python3
r"""
build_client.py - one command that turns a stock A2 / Orion Drift APK + OBB into a
client pointed at your own backend.

    python build_client.py --apk in.apk --obb in.obb ^
        --mothership http://192.168.1.50:8080 ^
        --dashboard  http://192.168.1.50:9000

It does the whole job:

  OBB   Rewrites  [OnlineSubsystemMothership] BaseUrl  in the packaged
        A2/Config/DefaultEngine.ini (an Oodle-compressed pak entry), re-encodes that
        entry as Zlib, relocates it to the end of the pak's data region, fixes the pak
        index/footer/hashes, and rebuilds the .obb zip.
        The Mothership URL may be any length.

  APK   Rewrites the Dashboard URL, a compile-time constant in lib/arm64-v8a/libUnreal.so
        (`https://api.oriondrift.net`). That one is patched in place inside the binary,
        so the new URL must be NO LONGER than the original - it is NUL-padded if shorter.
        The APK is then repacked, zipaligned and re-signed.

  Tools It finds zipalign / apksigner / keytool / adb automatically, including the copies
        Unity ships with its Android player, so nothing extra needs installing. A debug
        keystore is generated on first run if you do not pass one.

Re-signing means a different signature from the store build, so uninstall the original
before installing the output. Package name and version code are untouched, so the OBB
keeps its name and the pair still matches.

The app also refuses an OBB whose length differs from the one it shipped with: that size
is compiled into classes.dex (UE's OBBData/XAPKFile table). The build retargets it to the
OBB it just produced, so a resized OBB is accepted. --disable-obb-verify instead flips the
manifest's bVerifyOBBOnStartUp, skipping the check altogether.

  --install             push the results to a connected headset with adb
  --disable-obb-verify  skip the expansion-file check instead of retargeting the size
  --keep                keep the work directory for inspection
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import zipfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import a2urlpatch as pak_lib   # noqa: E402  (pak/obb parsing lives there)

DASHBOARD_DEFAULT = "https://api.oriondrift.net"
MOTHERSHIP_SECTION = "OnlineSubsystemMothership"
SO_ENTRY = "lib/arm64-v8a/libUnreal.so"
INI_ENTRY = "A2/Config/DefaultEngine.ini"

C_RESET, C_CYAN, C_GREEN, C_YELLOW, C_RED = "\033[0m", "\033[36m", "\033[32m", "\033[33m", "\033[31m"


def step(msg):  print(f"{C_CYAN}==>{C_RESET} {msg}", flush=True)
def info(msg):  print(f"    {msg}", flush=True)
def good(msg):  print(f"{C_GREEN}  ok{C_RESET} {msg}", flush=True)
def warn(msg):  print(f"{C_YELLOW}warn{C_RESET} {msg}", flush=True)
def die(msg):   raise SystemExit(f"{C_RED}error{C_RESET} {msg}")


# ---------------------------------------------------------------------------
# Toolchain discovery
# ---------------------------------------------------------------------------

def _unity_android_roots():
    for base in (r"C:\Program Files\Unity\Hub\Editor", r"C:\Program Files (x86)\Unity\Hub\Editor"):
        for ed in sorted(glob.glob(os.path.join(base, "*")), reverse=True):
            yield os.path.join(ed, r"Editor\Data\PlaybackEngines\AndroidPlayer")


def find_tools(sdk_hint: str | None):
    """Locate zipalign / apksigner / keytool / adb, preferring an explicit --sdk."""
    tools = {"zipalign": None, "apksigner": None, "keytool": None, "adb": None}

    def take(name, path):
        if path and os.path.isfile(path) and not tools[name]:
            tools[name] = path

    roots = []
    if sdk_hint:
        roots.append(sdk_hint)
    for env in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if os.environ.get(env):
            roots.append(os.environ[env])
    roots.append(os.path.expandvars(r"%LOCALAPPDATA%\Android\Sdk"))
    unity = list(_unity_android_roots())
    roots += [os.path.join(u, "SDK") for u in unity]

    for root in roots:
        for bt in sorted(glob.glob(os.path.join(root, "build-tools", "*")), reverse=True):
            take("zipalign", os.path.join(bt, "zipalign.exe"))
            take("apksigner", os.path.join(bt, "apksigner.bat"))
        take("adb", os.path.join(root, "platform-tools", "adb.exe"))
    for u in unity:
        take("keytool", os.path.join(u, r"OpenJDK\bin\keytool.exe"))
    for name in tools:
        if not tools[name]:
            take(name, shutil.which(name) or shutil.which(name + ".exe") or shutil.which(name + ".bat"))

    # apksigner is a .bat that shells out to java, so it needs JAVA_HOME. Unity ships a
    # JDK next to its Android player; keytool sitting in <jdk>\bin gives us its root.
    tools["java_home"] = os.environ.get("JAVA_HOME")
    if not tools["java_home"] and tools["keytool"]:
        cand = os.path.dirname(os.path.dirname(tools["keytool"]))
        if os.path.isfile(os.path.join(cand, "bin", "java.exe")):
            tools["java_home"] = cand
    if not tools["java_home"]:
        for u in unity:
            cand = os.path.join(u, "OpenJDK")
            if os.path.isfile(os.path.join(cand, "bin", "java.exe")):
                tools["java_home"] = cand
                break
    return tools


def java_env(tools: dict) -> dict:
    """Environment for the JDK-backed tools (apksigner, keytool)."""
    env = os.environ.copy()
    if tools.get("java_home"):
        env["JAVA_HOME"] = tools["java_home"]
        env["PATH"] = os.path.join(tools["java_home"], "bin") + os.pathsep + env.get("PATH", "")
    return env


# ---------------------------------------------------------------------------
# OBB / pak
# ---------------------------------------------------------------------------

def patch_ini_text(text: str, url: str) -> str:
    """Set BaseUrl inside [OnlineSubsystemMothership]. Length is unconstrained."""
    pattern = re.compile(
        r"(\[" + re.escape(MOTHERSHIP_SECTION) + r"\][^\[]*?BaseUrl\s*=\s*)\"[^\"]*\"",
        re.IGNORECASE | re.DOTALL)
    new_text, n = pattern.subn(lambda m: m.group(1) + f'"{url}"', text, count=1)
    if n == 0:
        die(f"could not find BaseUrl inside [{MOTHERSHIP_SECTION}] in {INI_ENTRY}")
    return new_text


def rebuild_pak(src_pak: str, dst_pak: str, entry_name: str, new_blob: bytes) -> None:
    """
    Write dst_pak: src_pak with `entry_name` replaced by new_blob.

    The replacement is re-encoded with Zlib and appended at the end of the data region,
    just before the index. Only that entry moves; every other entry keeps its offset, so
    the encoded index records, the path-hash index and the directory index all stay
    structurally identical - only a handful of fixed-width fields change.
    """
    win = pak_lib.Window(path=src_pak, start=0, size=os.path.getsize(src_pak),
                         name=os.path.basename(src_pak))
    pak = pak_lib.Pak(win, pak_lib.Oodle())
    e = pak.entry(entry_name)
    h = pak.read_header(e)

    blocks_in = [new_blob[i:i + h["block_size"]] for i in range(0, len(new_blob), h["block_size"])] or [b""]
    if len(blocks_in) != max(1, e.nblocks):
        die(f"{entry_name}: block count would change ({e.nblocks} -> {len(blocks_in)}); "
            f"the new content must stay within {e.nblocks * h['block_size']:,} bytes")
    zblocks = [zlib.compress(b, 9) for b in blocks_in]
    payload = b"".join(zblocks)
    zlib_idx = _method_index(pak, "Zlib")

    # Inline FPakEntry header, same field order as the original.
    hdr_blocks_rel = []
    cur = None
    header_len = h["header_len"]
    cur = header_len
    for z in zblocks:
        hdr_blocks_rel.append((cur, cur + len(z)))
        cur += len(z)

    hdr = bytearray()
    hdr += struct.pack("<q", 0)                       # Offset: UnrealPak stores 0 here
    hdr += struct.pack("<q", len(payload))            # Size (compressed)
    hdr += struct.pack("<q", len(new_blob))           # UncompressedSize
    hdr += struct.pack("<I", zlib_idx)
    hdr += hashlib.sha1(payload).digest()
    hdr += struct.pack("<i", len(zblocks))
    for s, en in hdr_blocks_rel:
        hdr += struct.pack("<qq", s, en)
    hdr += bytes([h["flags"]])
    hdr += struct.pack("<I", h["block_size"])
    if len(hdr) != header_len:
        die(f"rebuilt entry header is {len(hdr)} bytes, expected {header_len}")

    src = open(src_pak, "rb").read()
    new_entry_off = pak.index_offset
    entry_bytes = bytes(hdr) + payload
    delta = len(entry_bytes)

    # ---- primary index: fix the moved entry's record + the sub-index offsets ----
    index = bytearray(src[pak.index_offset:pak.index_offset + pak.index_size])
    _patch_encoded_record(index, pak, e, new_entry_off, len(payload), len(new_blob),
                          zlib_idx, [len(z) for z in zblocks])
    _shift_subindex_offsets(index, pak, delta)

    tail = bytearray(src[pak.index_offset:])          # index + sub-indexes + footer
    tail[0:pak.index_size] = index

    # ---- footer: index offset moves, its hash changes, Zlib joins the table ----
    foot = pak.magic_off - pak.index_offset           # footer position inside the tail
    struct.pack_into("<Q", tail, foot + 8, pak.index_offset + delta)
    tail[foot + 24: foot + 44] = hashlib.sha1(bytes(index)).digest()
    moff = foot + 44 + (zlib_idx - 1) * pak_lib.COMPRESSION_NAME_LEN
    tail[moff: moff + pak_lib.COMPRESSION_NAME_LEN] = b"Zlib".ljust(pak_lib.COMPRESSION_NAME_LEN, b"\0")

    with open(dst_pak, "wb") as f:
        f.write(src[:pak.index_offset])
        f.write(entry_bytes)
        f.write(bytes(tail))
    info(f"pak {os.path.getsize(src_pak):,} -> {os.path.getsize(dst_pak):,} bytes "
         f"(entry relocated to 0x{new_entry_off:X}, +{delta:,})")


def _method_index(pak: "pak_lib.Pak", name: str) -> int:
    for i, m in enumerate(pak.methods):
        if m.lower() == name.lower():
            return i + 1
    for i, m in enumerate(pak.methods):
        if not m:
            return i + 1
    die("no free compression-method slot in the pak footer")


def _patch_encoded_record(index: bytearray, pak, e, offset, size, usize, cmi, block_sizes):
    """Rewrite one encoded entry in place. Byte length must not change."""
    base = pak.encoded_off_in_index + e.encoded_off
    o = base
    v = struct.unpack_from("<I", index, o)[0]
    v = (v & ~(0x3F << 23)) | ((cmi & 0x3F) << 23)
    struct.pack_into("<I", index, o, v); o += 4
    if (v & 0x3F) == 0x3F:
        o += 4
    off32, usz32, sz32 = (v >> 31) & 1, (v >> 30) & 1, (v >> 29) & 1
    for value, is32 in ((offset, off32), (usize, usz32), (size, sz32)):
        if is32:
            if value > 0xFFFFFFFF:
                die("value no longer fits a 32-bit index field; pak is too large for in-place edits")
            struct.pack_into("<I", index, o, value); o += 4
        else:
            struct.pack_into("<Q", index, o, value); o += 8
    if e.blocks:
        if len(e.blocks) != len(block_sizes):
            die("block count changed in the index record")
        for bsz in block_sizes:
            struct.pack_into("<I", index, o, bsz); o += 4


def _shift_subindex_offsets(index: bytearray, pak, delta: int) -> None:
    """PathHashIndex / FullDirectoryIndex are absolute file offsets; both move by delta."""
    r = pak_lib.Reader(bytes(index))
    r.fstring(); r.i32(); r.u64()
    if r.i32():
        struct.pack_into("<q", index, r.o, struct.unpack_from("<q", index, r.o)[0] + delta)
        r.i64(); r.i64(); r.raw(20)
    if r.i32():
        struct.pack_into("<q", index, r.o, struct.unpack_from("<q", index, r.o)[0] + delta)
        r.i64(); r.i64(); r.raw(20)


def build_obb(src_obb: str, dst_obb: str, mothership_url: str, work: str) -> None:
    step(f"OBB: {os.path.basename(src_obb)}")
    with zipfile.ZipFile(src_obb) as z:
        infos = z.infolist()
        comment = z.comment
        pak_names = [i.filename for i in infos if i.filename.endswith(".pak")]
        if not pak_names:
            die("no .pak inside the obb")
        pak_name = pak_names[0]
        info(f"pak entry: {pak_name}")
        src_pak = os.path.join(work, "orig.pak")
        with z.open(pak_name) as s, open(src_pak, "wb") as d:
            shutil.copyfileobj(s, d, 1 << 24)

    win = pak_lib.Window(path=src_pak, start=0, size=os.path.getsize(src_pak), name=pak_name)
    pak = pak_lib.Pak(win, pak_lib.Oodle())
    if INI_ENTRY not in pak.files:
        die(f"{INI_ENTRY} not found in {pak_name}")
    e = pak.entry(INI_ENTRY)
    text = pak.extract(e).decode("utf-8", "surrogateescape")
    current = re.search(r"\[" + MOTHERSHIP_SECTION + r"\][^\[]*?BaseUrl\s*=\s*\"([^\"]*)\"",
                        text, re.IGNORECASE | re.DOTALL)
    info(f"current Mothership BaseUrl: {current.group(1) if current else '(not found)'}")
    new_text = patch_ini_text(text, mothership_url)
    new_blob = new_text.encode("utf-8", "surrogateescape")
    info(f"new     Mothership BaseUrl: {mothership_url}")
    info(f"ini {len(text):,} -> {len(new_blob):,} bytes")

    new_pak = os.path.join(work, "new.pak")
    rebuild_pak(src_pak, new_pak, INI_ENTRY, new_blob)

    # Sanity: re-open the rebuilt pak and read the value back.
    win2 = pak_lib.Window(path=new_pak, start=0, size=os.path.getsize(new_pak), name=pak_name)
    pak2 = pak_lib.Pak(win2, pak_lib.Oodle())
    check = pak2.extract(pak2.entry(INI_ENTRY)).decode("utf-8", "surrogateescape")
    if mothership_url not in check:
        die("rebuilt pak does not contain the new URL - aborting")
    if hashlib.sha1(win2.read(pak2.index_offset, pak2.index_size)).digest() != pak2.index_hash:
        die("rebuilt pak index hash mismatch - aborting")
    good("rebuilt pak verifies (index hash OK, new URL present)")

    step("OBB: repacking")
    t0 = time.time()
    with zipfile.ZipFile(src_obb) as zin, zipfile.ZipFile(dst_obb, "w", zipfile.ZIP_STORED,
                                                          allowZip64=True) as zout:
        zout.comment = comment
        for i in zin.infolist():
            zi = zipfile.ZipInfo(i.filename, date_time=i.date_time)
            zi.compress_type = zipfile.ZIP_STORED
            zi.external_attr = i.external_attr
            if i.filename == pak_name:
                with open(new_pak, "rb") as s, zout.open(zi, "w") as d:
                    shutil.copyfileobj(s, d, 1 << 24)
            else:
                with zin.open(i) as s, zout.open(zi, "w") as d:
                    shutil.copyfileobj(s, d, 1 << 24)
    good(f"{dst_obb} ({os.path.getsize(dst_obb):,} bytes, {time.time()-t0:.0f}s)")


# ---------------------------------------------------------------------------
# APK: the expected OBB size baked into classes.dex
# ---------------------------------------------------------------------------

def patch_dex_obb_size(dex: bytearray, old_size: int, new_size: int) -> int:
    """
    Retarget the OBB size the app expects.

    The packaged size is compiled into classes.dex as a `const-wide/32` literal and the
    app refuses an expansion file whose length does not match it - which is why a rebuilt
    OBB otherwise has to be exactly the same size.

    It lives in UE's generated OBBData class, which R8 renamed:
      Lcom/<pkg>/a;-><clinit>          builds the XAPKFile[] table
      Lcom/<pkg>/a$a;                  the record: boolean a, String b, long c  <- the size
    The manifest meta-data bVerifyOBBOnStartUp is the on/off switch for the check that
    reads it; --disable-obb-verify flips that instead (see set_verify_obb_flag). Nothing
    in the native libraries validates the OBB - libUnreal.so only carries the
    AndroidRuntimeSettings property names (bDisableVerifyOBBOnStartUp and friends), which
    are packaging-time settings that decide what UBT writes into the manifest.

    The literal is 32 bits, sign-extended to a long, so it addresses OBBs up to 2 GiB.
    Patching it keeps classes.dex exactly the same length; the dex checksum and signature
    are recomputed afterwards.

    Returns the number of sites patched.
    """
    if not -0x80000000 <= new_size <= 0x7FFFFFFF:
        die(f"new OBB is {new_size:,} bytes, too large for the 32-bit size literal in the dex")

    old_le = struct.pack("<i", old_size)
    sites = []
    start = 0
    while True:
        i = dex.find(old_le, start)
        if i < 0:
            break
        # 0x17 = const-wide/32 vAA, #+BBBBBBBB : opcode, register, then the literal
        if i >= 2 and dex[i - 2] == 0x17:
            sites.append(i)
        start = i + 1
    for i in sites:
        dex[i:i + 4] = struct.pack("<i", new_size)
    return len(sites)


def fix_dex_hashes(dex: bytearray) -> None:
    """Recompute the dex SHA-1 signature and Adler-32 checksum (both fixed-width)."""
    dex[12:32] = hashlib.sha1(bytes(dex[32:])).digest()
    struct.pack_into("<I", dex, 8, zlib.adler32(bytes(dex[12:])) & 0xFFFFFFFF)


VERIFY_META = "com.epicgames.unreal.GameActivity.bVerifyOBBOnStartUp"


def _axml_strings(buf: bytes) -> list[str]:
    off, strings = 8, []
    while off < len(buf):
        ctype, hsize, csize = struct.unpack_from("<HHI", buf, off)
        if ctype == 0x0001:                                   # RES_STRING_POOL_TYPE
            count, _styc, flags, sstart, _sty = struct.unpack_from("<IIIII", buf, off + 8)
            utf8 = bool(flags & (1 << 8))
            base = off + sstart
            for i in range(count):
                p = base + struct.unpack_from("<I", buf, off + 28 + 4 * i)[0]
                if utf8:
                    strings.append(buf[p + 2:p + 2 + buf[p]].decode("utf-8", "replace"))
                else:
                    n = struct.unpack_from("<H", buf, p)[0]
                    strings.append(buf[p + 2:p + 2 + n * 2].decode("utf-16-le", "replace"))
            return strings
        off += csize
    return strings


def set_verify_obb_flag(manifest: bytearray, enabled: bool) -> bool:
    """
    Flip the <meta-data android:name="...bVerifyOBBOnStartUp"> value in the binary manifest.

    That meta-data is what GameActivity reads to decide whether to run the expansion-file
    check at all; the size it compares against comes from the dex (see patch_dex_obb_size).
    Turning it off skips existence and size checking together, which is the blunter of the
    two fixes - retargeting the dex size keeps the check working.
    The value is a TYPE_INT_BOOLEAN, so this is a 4-byte write and the manifest keeps its
    exact length. Returns True if the flag was found.
    """
    strings = _axml_strings(bytes(manifest))
    off = 8
    while off < len(manifest):
        ctype, hsize, csize = struct.unpack_from("<HHI", manifest, off)
        if ctype == 0x0102:                                   # RES_XML_START_ELEMENT_TYPE
            name_i = struct.unpack_from("<I", manifest, off + 20)[0]
            count = struct.unpack_from("<H", manifest, off + 28)[0]
            attrs = off + 36
            if 0 <= name_i < len(strings) and strings[name_i] == "meta-data":
                found_name = False
                value_at = None
                for i in range(count):
                    a = attrs + i * 20
                    nm_i, raw_i = struct.unpack_from("<II", manifest, a + 4)
                    nm = strings[nm_i] if 0 <= nm_i < len(strings) else ""
                    raw = strings[raw_i] if 0 <= raw_i < len(strings) else ""
                    if nm == "name" and raw == VERIFY_META:
                        found_name = True
                    elif nm == "value":
                        value_at = a
                if found_name and value_at is not None:
                    if manifest[value_at + 15] != 0x12:        # TYPE_INT_BOOLEAN
                        return False
                    struct.pack_into("<I", manifest, value_at + 16, 0xFFFFFFFF if enabled else 0)
                    return True
        off += csize
    return False


# ---------------------------------------------------------------------------
# APK
# ---------------------------------------------------------------------------

def patch_so(data: bytearray, old: bytes, new: bytes) -> int:
    """Overwrite a C string constant in place; the new value must fit and is NUL padded."""
    if len(new) > len(old):
        die(f"dashboard URL is {len(new)} bytes but only {len(old)} are available in the binary "
            f"(the original is {old.decode()!r}).\n"
            f"       It is patched in place inside libUnreal.so, so it cannot grow. "
            f"Use at most {len(old)} characters.")
    n = 0
    start = 0
    while True:
        i = data.find(old, start)
        if i < 0:
            break
        data[i:i + len(old)] = new + b"\0" * (len(old) - len(new))
        n += 1
        start = i + len(old)
    return n


def build_apk(src_apk: str, dst_apk: str, dashboard_url: str, work: str, tools: dict,
              keystore: str | None, obb_sizes: tuple[int, int] | None = None,
              disable_obb_verify: bool = False) -> None:
    step(f"APK: {os.path.basename(src_apk)}")
    with zipfile.ZipFile(src_apk) as z:
        if SO_ENTRY not in z.namelist():
            die(f"{SO_ENTRY} not found in the apk")
        so = bytearray(z.read(SO_ENTRY))
    info(f"{SO_ENTRY}: {len(so):,} bytes")
    hits = patch_so(so, DASHBOARD_DEFAULT.encode(), dashboard_url.encode())
    if hits == 0:
        die(f"{DASHBOARD_DEFAULT!r} not found in the binary - is this the right build?")
    info(f"dashboard URL: {DASHBOARD_DEFAULT} -> {dashboard_url}  ({hits} site{'s' if hits > 1 else ''})")

    # Retarget the OBB size compiled into classes.dex, so a resized OBB is accepted.
    dex = None
    if obb_sizes:
        old_size, new_size = obb_sizes
        with zipfile.ZipFile(src_apk) as z:
            dex = bytearray(z.read("classes.dex"))
        n = patch_dex_obb_size(dex, old_size, new_size)
        if n == 0:
            warn(f"no const-wide/32 literal equal to the original OBB size ({old_size:,}) was "
                 "found in classes.dex; leaving it alone")
            dex = None
        else:
            fix_dex_hashes(dex)
            info(f"classes.dex: expected OBB size {old_size:,} -> {new_size:,} "
                 f"({n} site{'s' if n > 1 else ''}, checksum + signature rebuilt)")

    manifest = None
    if disable_obb_verify:
        with zipfile.ZipFile(src_apk) as z:
            manifest = bytearray(z.read("AndroidManifest.xml"))
        if set_verify_obb_flag(manifest, False):
            info("AndroidManifest.xml: bVerifyOBBOnStartUp true -> false "
                 "(the expansion-file check is skipped entirely)")
        else:
            warn("bVerifyOBBOnStartUp meta-data not found; manifest left alone")
            manifest = None

    raw = os.path.join(work, "unsigned.apk")
    step("APK: repacking")
    with zipfile.ZipFile(src_apk) as zin, zipfile.ZipFile(raw, "w", allowZip64=True) as zout:
        for i in zin.infolist():
            # Signature files describe the old contents; apksigner writes fresh ones.
            if i.filename.startswith("META-INF/") and i.filename.upper().endswith((".SF", ".RSA", ".DSA", ".EC")):
                continue
            zi = zipfile.ZipInfo(i.filename, date_time=i.date_time)
            zi.compress_type = i.compress_type
            zi.external_attr = i.external_attr
            if i.filename == SO_ENTRY:
                data = so
            elif dex is not None and i.filename == "classes.dex":
                data = dex
            elif manifest is not None and i.filename == "AndroidManifest.xml":
                data = manifest
            else:
                data = zin.read(i)
            zout.writestr(zi, bytes(data))
    info(f"repacked ({os.path.getsize(raw):,} bytes)")

    aligned = os.path.join(work, "aligned.apk")
    if tools["zipalign"]:
        step("APK: zipalign")
        subprocess.run([tools["zipalign"], "-p", "-f", "4", raw, aligned], check=True,
                       stdout=subprocess.DEVNULL)
        good("aligned (4 bytes, -p)")
    else:
        warn("zipalign not found; skipping alignment")
        shutil.copyfile(raw, aligned)

    if tools["apksigner"] and not tools.get("java_home"):
        warn("apksigner needs a JDK but none was found; set JAVA_HOME or pass --sdk")
    if not tools["apksigner"]:
        shutil.copyfile(aligned, dst_apk)
        warn("apksigner not found - the output is UNSIGNED and will not install.")
        warn("Install Android build-tools, or sign it yourself.")
        return

    ks = keystore or os.path.join(os.path.dirname(os.path.abspath(__file__)), "halcyon-debug.keystore")
    if not os.path.isfile(ks):
        if not tools["keytool"]:
            die("no keystore and keytool was not found; pass --keystore <file>")
        step("APK: generating a debug keystore")
        subprocess.run([tools["keytool"], "-genkeypair", "-v", "-keystore", ks,
                        "-alias", "halcyon", "-keyalg", "RSA", "-keysize", "2048",
                        "-validity", "10000", "-storepass", "halcyon", "-keypass", "halcyon",
                        "-dname", "CN=HalcyonA2, OU=PrivateServer, O=Halcyon, C=US"],
                       check=True, env=java_env(tools),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        good(f"{ks}  (store/key password: halcyon)")

    step("APK: signing")
    subprocess.run([tools["apksigner"], "sign", "--ks", ks,
                    "--ks-pass", "pass:halcyon", "--key-pass", "pass:halcyon",
                    "--out", dst_apk, aligned], check=True, env=java_env(tools))
    verify = subprocess.run([tools["apksigner"], "verify", "--print-certs", dst_apk],
                            capture_output=True, text=True, env=java_env(tools))
    if verify.returncode != 0:
        die(f"signature verification failed:\n{verify.stdout}{verify.stderr}")
    good(f"{dst_apk} ({os.path.getsize(dst_apk):,} bytes, signature verified)")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Interactive mode - run with no arguments and it just asks
# ---------------------------------------------------------------------------

def _clean_path(s: str) -> str:
    """Accept drag-and-dropped paths: strip quotes and stray whitespace."""
    return s.strip().strip('"').strip("'").strip()


def ask(question: str, default: str | None = None, hint: str | None = None) -> str:
    if hint:
        print(f"    {hint}")
    suffix = f" [{default}]" if default else ""
    while True:
        answer = input(f"{C_CYAN}?{C_RESET} {question}{suffix}: ").strip()
        if answer:
            return answer
        if default is not None:
            return default
        print("    (an answer is needed)")


def ask_yes_no(question: str, default: bool = False, hint: str | None = None) -> bool:
    if hint:
        print(f"    {hint}")
    suffix = " [Y/n]" if default else " [y/N]"
    while True:
        answer = input(f"{C_CYAN}?{C_RESET} {question}{suffix}: ").strip().lower()
        if not answer:
            return default
        if answer in ("y", "yes"):
            return True
        if answer in ("n", "no"):
            return False
        print("    (please answer y or n)")


def find_candidates(patterns: list[str]) -> list[str]:
    """Look for input files near the tool and near wherever it was run from."""
    roots, seen = [], set()
    here = os.path.dirname(os.path.abspath(__file__))
    for base in (os.getcwd(), here, os.path.dirname(here), os.path.dirname(os.path.dirname(here))):
        base = os.path.abspath(base)
        if base not in seen:
            seen.add(base)
            roots.append(base)
    out = []
    for root in roots:
        for pat in patterns:
            for hit in sorted(glob.glob(os.path.join(root, pat))):
                if os.path.isfile(hit) and hit not in out:
                    out.append(hit)
    return out


def ask_file(question: str, patterns: list[str], hint: str) -> str | None:
    """Offer whatever was found nearby, numbered; or take a typed/dropped path."""
    found = find_candidates(patterns)
    print()
    print(f"    {hint}")
    if found:
        for i, f in enumerate(found, 1):
            size = os.path.getsize(f)
            print(f"      {i}) {os.path.basename(f)}   ({size / 1024 / 1024:,.0f} MB)")
            print(f"         {os.path.dirname(f)}")
        print(f"      s) skip - do not touch this file")
    while True:
        answer = _clean_path(input(f"{C_CYAN}?{C_RESET} {question}"
                                   f"{' [1]' if found else ''}: "))
        if not answer and found:
            return found[0]
        if answer.lower() in ("s", "skip"):
            return None
        if answer.isdigit() and found and 1 <= int(answer) <= len(found):
            return found[int(answer) - 1]
        if os.path.isfile(answer):
            return answer
        print(f"    (no such file: {answer})")


def ask_url(question: str, current: str, max_len: int | None, hint: str) -> str:
    print()
    print(f"    {hint}")
    print(f"    the stock client uses: {current}")
    if max_len:
        print(f"    yours must be {max_len} characters or fewer (it is patched in place)")
    while True:
        url = input(f"{C_CYAN}?{C_RESET} {question}: ").strip()
        if not url:
            print("    (an answer is needed)")
            continue
        if not url.startswith(("http://", "https://")):
            if not ask_yes_no(f"    '{url}' has no http:// or https:// prefix - use it anyway?", False):
                continue
        if max_len and len(url) > max_len:
            print(f"    (that is {len(url)} characters, {len(url) - max_len} too many - "
                  f"try an ip and port, e.g. http://192.168.1.50:9000)")
            continue
        return url


def interactive(args):
    """Fill in `args` by asking, so no flags are needed."""
    print()
    print(f"{C_CYAN}A2 client builder{C_RESET}")
    print("Point a stock Orion Drift client at your own backend.")
    print("Press Enter to take the [default] on any question.")

    args.apk = ask_file("which APK?", ["*.apk"],
                        "The APK holds the Dashboard URL, inside libUnreal.so.")
    args.obb = ask_file("which OBB?", ["main.*.obb", "*.obb"],
                        "The OBB holds the Mothership URL, inside the packaged DefaultEngine.ini.")
    if not args.apk and not args.obb:
        die("nothing to do - both files were skipped")

    if args.obb:
        args.mothership = ask_url("new Mothership URL", "https://aa-mothership.com", None,
                                  "Where the client logs in / fetches player data.")
    if args.apk:
        default_dash = None
        if args.mothership and len(args.mothership) <= len(DASHBOARD_DEFAULT):
            print()
            if ask_yes_no(f"use the same host for the Dashboard ({args.mothership})?", True,
                          "The Dashboard serves station/deployment config."):
                default_dash = args.mothership
        args.dashboard = default_dash or ask_url(
            "new Dashboard URL", DASHBOARD_DEFAULT, len(DASHBOARD_DEFAULT),
            "Where the client fetches station and deployment config.")

    print()
    args.out = ask("output folder", args.out or "out")

    if args.apk and args.obb:
        print()
        print("    The app checks the OBB's exact byte size against a number baked into the")
        print("    APK. Rebuilding the OBB changes that size, so the number is retargeted for")
        print("    you. You can instead switch the check off entirely.")
        args.disable_obb_verify = ask_yes_no("switch the OBB check off completely?", False)
    elif args.obb and not args.apk:
        print()
        warn("Rebuilding the OBB without rebuilding the APK leaves the size check stale;")
        warn("the client will reject the new OBB. Build both together if you can.")

    if not args.apk:
        args.no_obb_size_patch = True

    print()
    args.install = ask_yes_no("install to a connected headset when finished?", False,
                              "Needs adb, and the original app will be uninstalled first.")

    print()
    print(f"{C_CYAN}summary{C_RESET}")
    info(f"apk        : {args.apk or '(skipped)'}")
    info(f"obb        : {args.obb or '(skipped)'}")
    info(f"mothership : {args.mothership or '(unchanged)'}")
    info(f"dashboard  : {args.dashboard or '(unchanged)'}")
    info(f"output     : {args.out}")
    if args.disable_obb_verify:
        info("obb check  : switched off")
    if args.install:
        info("install    : yes")
    print()
    if not ask_yes_no("build it?", True):
        raise SystemExit("cancelled")

    # So it can be repeated without the questions next time.
    cmd = [f'python build_client.py']
    if args.apk:         cmd.append(f'--apk "{args.apk}"')
    if args.obb:         cmd.append(f'--obb "{args.obb}"')
    if args.mothership:  cmd.append(f'--mothership {args.mothership}')
    if args.dashboard:   cmd.append(f'--dashboard {args.dashboard}')
    cmd.append(f'--out "{args.out}"')
    if args.disable_obb_verify: cmd.append("--disable-obb-verify")
    if args.install:            cmd.append("--install")
    print()
    info("same build without the questions:")
    info("  " + " ".join(cmd))
    print()
    return args


def obb_package_and_name(obb_path: str) -> tuple[str, str]:
    """main.<versionCode>.<package>.obb -> (package, filename)"""
    name = os.path.basename(obb_path)
    m = re.match(r"main\.(\d+)\.(.+)\.obb$", name)
    if not m:
        return "", name
    return m.group(2), name


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[1],
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--apk", help="input .apk")
    p.add_argument("--obb", help="input .obb")
    p.add_argument("--mothership", help="new Mothership BaseUrl (any length)")
    p.add_argument("--dashboard", help=f"new Dashboard API url (max {len(DASHBOARD_DEFAULT)} chars)")
    p.add_argument("--out", default=None, help="output directory (default: out)")
    p.add_argument("--keystore", help="keystore to sign with (default: a generated debug one)")
    p.add_argument("--sdk", help="Android SDK root, if it is somewhere unusual")
    p.add_argument("--install", action="store_true", help="adb install + push the obb when done")
    p.add_argument("--keep", action="store_true", help="keep the work directory")
    p.add_argument("--no-obb-size-patch", action="store_true",
                   help="do not retarget the expected OBB size in classes.dex")
    p.add_argument("--disable-obb-verify", action="store_true",
                   help="also flip bVerifyOBBOnStartUp to false, skipping the check entirely")
    p.add_argument("--obb-size", metavar="OLD:NEW",
                   help="retarget the dex OBB size when patching an apk without an obb")
    p.add_argument("-i", "--interactive", action="store_true",
                   help="ask questions instead of taking flags (the default with no flags)")
    args = p.parse_args(argv)

    # No flags at all? Then just ask. Nobody should have to memorise this interface.
    if args.interactive or not (args.apk or args.obb):
        try:
            args = interactive(args)
        except (KeyboardInterrupt, EOFError):
            raise SystemExit("cancelled")

    if not args.apk and not args.obb:
        p.error("give --apk, --obb, or both")
    if args.obb and not args.mothership:
        p.error("--obb needs --mothership")
    if args.apk and not args.dashboard:
        p.error("--apk needs --dashboard")
    if args.dashboard and len(args.dashboard) > len(DASHBOARD_DEFAULT):
        die(f"--dashboard is {len(args.dashboard)} characters; the binary has room for "
            f"{len(DASHBOARD_DEFAULT)} (it is patched in place). Shorten it, e.g. use an "
            f"IP and port: http://192.168.1.50:9000")

    args.out = args.out or "out"
    os.makedirs(args.out, exist_ok=True)
    work = os.path.join(args.out, "_work")
    os.makedirs(work, exist_ok=True)

    tools = find_tools(args.sdk)
    step("toolchain")
    for k in ("zipalign", "apksigner", "keytool", "adb", "java_home"):
        info(f"{k:<10} {tools.get(k) or '(not found)'}")

    out_apk = out_obb = None
    obb_sizes = None
    if args.obb:
        pkg, name = obb_package_and_name(args.obb)
        out_obb = os.path.join(args.out, name)
        build_obb(args.obb, out_obb, args.mothership, work)
        if not args.no_obb_size_patch:
            obb_sizes = (os.path.getsize(args.obb), os.path.getsize(out_obb))
    elif args.obb_size and args.apk:
        # patching an apk on its own, against an obb built earlier
        obb_sizes = tuple(int(x) for x in args.obb_size.split(":", 1))
    if args.apk:
        out_apk = os.path.join(args.out, os.path.basename(args.apk).replace(".apk", "") + "-halcyon.apk")
        build_apk(args.apk, out_apk, args.dashboard, work, tools, args.keystore, obb_sizes,
                  args.disable_obb_verify)
    elif obb_sizes and obb_sizes[0] != obb_sizes[1]:
        warn(f"the OBB changed size ({obb_sizes[0]:,} -> {obb_sizes[1]:,}) but no --apk was given.")
        warn("The app compares the OBB length against a constant in classes.dex and will")
        warn("reject a mismatch, so rebuild the apk too (or pass --obb-size old:new to it).")

    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)

    print()
    step("done")
    if out_apk:
        info(f"apk : {out_apk}")
    if out_obb:
        info(f"obb : {out_obb}")

    pkg = obb_package_and_name(args.obb)[0] if args.obb else "com.AnotherAxiom.A2"
    if args.install:
        if not tools["adb"]:
            warn("adb not found; skipping install")
        else:
            step("installing")
            subprocess.run([tools["adb"], "uninstall", pkg], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            subprocess.run([tools["adb"], "install", "-r", out_apk], check=True)
            if out_obb:
                dest = f"/sdcard/Android/obb/{pkg}/{os.path.basename(out_obb)}"
                subprocess.run([tools["adb"], "shell", "mkdir", "-p",
                                f"/sdcard/Android/obb/{pkg}"], check=False)
                subprocess.run([tools["adb"], "push", out_obb, dest], check=True)
            good("installed")
    else:
        print()
        info("to install:")
        info(f"  adb uninstall {pkg}")
        if out_apk:
            info(f"  adb install -r \"{out_apk}\"")
        if out_obb:
            info(f"  adb push \"{out_obb}\" /sdcard/Android/obb/{pkg}/{os.path.basename(out_obb)}")
        info("(the signature differs from the store build, so the original must be removed first)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
