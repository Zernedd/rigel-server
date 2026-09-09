#!/usr/bin/env python3
r"""
patch_apk_cmdline.py - rewrite an A2 / Orion Drift Android APK's UE command line.

A UE Android package reads its startup command line from `assets/UECommandLine.txt` inside the
APK. In the stock build that file is exactly:

    -project="../../../A2/A2.uproject"

which is where the internal command line people quote comes from. This tool appends (or replaces)
arguments there, repacks, zipaligns and re-signs, so the headset build starts with the arguments
you want without anyone typing anything.

    python patch_apk_cmdline.py --apk in.apk --connect 192.168.1.29:7777
    python patch_apk_cmdline.py --apk in.apk --args "-connectToServerByIPAndPort=1.2.3.4:7777 -httpproxy=1.2.3.4:8888"

Only the APK is touched - the OBB is untouched and still matches, so no expansion-file size
juggling is needed.

Re-signing changes the APK signature, so uninstall the existing build on the headset first.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_client import find_tools, java_env, step, info, good, warn, die  # reuse the toolchain finder

CMDLINE_ENTRY = "assets/UECommandLine.txt"


def read_cmdline(apk: str) -> str:
    with zipfile.ZipFile(apk) as z:
        if CMDLINE_ENTRY not in z.namelist():
            die(f"{CMDLINE_ENTRY} not present in {apk} - is this a UE Android package?")
        return z.read(CMDLINE_ENTRY).decode("utf-8", "replace")


def build_new_cmdline(original: str, extra: str, replace: bool) -> str:
    original = original.strip()
    if replace:
        return extra.strip()
    # Drop any prior copy of each switch we are adding, so re-running does not stack duplicates.
    kept = []
    incoming_keys = {a.split("=", 1)[0].lower() for a in extra.split() if a.startswith("-")}
    for tok in original.split():
        key = tok.split("=", 1)[0].lower()
        if key in incoming_keys:
            continue
        kept.append(tok)
    return " ".join(kept + extra.split())


def repack(src_apk: str, dst_apk: str, new_cmdline: str, work: str) -> None:
    """Copy every entry across, substituting the command line file."""
    tmp = os.path.join(work, "unsigned.apk")
    payload = new_cmdline.encode("utf-8")
    with zipfile.ZipFile(src_apk) as zin, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = payload if item.filename == CMDLINE_ENTRY else zin.read(item.filename)
            # Preserve STORED for entries that must stay uncompressed (libs, obb-ish assets):
            # re-deflating those can break UE's direct-mmap loading and zipalign expectations.
            zi = zipfile.ZipInfo(item.filename, date_time=item.date_time)
            zi.compress_type = item.compress_type
            zi.external_attr = item.external_attr
            zi.internal_attr = item.internal_attr
            zi.create_system = item.create_system
            zout.writestr(zi, data)
    shutil.move(tmp, dst_apk)


def align_and_sign(apk: str, tools: dict, keystore: str | None, work: str) -> None:
    env = java_env(tools)
    aligned = os.path.join(work, "aligned.apk")

    step("zipalign")
    subprocess.run([tools["zipalign"], "-p", "-f", "4", apk, aligned], check=True, env=env)
    shutil.move(aligned, apk)
    good("aligned")

    ks = keystore or os.path.join(os.path.dirname(os.path.abspath(__file__)), "halcyon-debug.keystore")
    if not os.path.exists(ks):
        step("generating a debug keystore")
        subprocess.run([tools["keytool"], "-genkeypair", "-v", "-keystore", ks,
                        "-alias", "halcyon", "-keyalg", "RSA", "-keysize", "2048",
                        "-validity", "10000", "-storepass", "halcyon", "-keypass", "halcyon",
                        "-dname", "CN=Halcyon, OU=Dev, O=Halcyon, L=., S=., C=US"],
                       check=True, env=env)
        good(ks)

    step("signing")
    subprocess.run([tools["apksigner"], "sign",
                    "--ks", ks, "--ks-pass", "pass:halcyon", "--key-pass", "pass:halcyon",
                    "--v1-signing-enabled", "true", "--v2-signing-enabled", "true",
                    apk], check=True, env=env)
    good("signed")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Patch the UE command line inside an A2 Android APK.")
    ap.add_argument("--apk", required=True, help="input .apk")
    ap.add_argument("--out", help="output .apk (default: <input>-cmdline.apk)")
    ap.add_argument("--connect", help="shorthand for -connectToServerByIPAndPort=<ip:port>")
    ap.add_argument("--args", help="raw arguments to append")
    ap.add_argument("--replace", action="store_true",
                    help="replace the whole command line instead of appending (drops -project=, rarely wanted)")
    ap.add_argument("--keystore", help="keystore to sign with (default: a generated debug one)")
    ap.add_argument("--sdk", help="Android SDK root, if it is somewhere unusual")
    ap.add_argument("--show", action="store_true", help="just print the current command line and exit")
    a = ap.parse_args(argv)

    if not os.path.isfile(a.apk):
        die(f"no such file: {a.apk}")

    current = read_cmdline(a.apk)
    info(f"current command line: {current.strip()!r}")
    if a.show:
        return 0

    extra_parts = []
    if a.connect:
        extra_parts.append(f"-connectToServerByIPAndPort={a.connect}")
    if a.args:
        extra_parts.append(a.args)
    if not extra_parts:
        die("nothing to do - pass --connect and/or --args (or --show)")
    extra = " ".join(extra_parts)

    new_cmdline = build_new_cmdline(current, extra, a.replace)
    info(f"new command line:     {new_cmdline!r}")

    out = a.out or os.path.splitext(a.apk)[0] + "-cmdline.apk"
    tools = find_tools(a.sdk)
    work = tempfile.mkdtemp(prefix="a2cmdline-")
    try:
        step(f"repacking -> {out}")
        repack(a.apk, out, new_cmdline, work)
        good(f"{os.path.getsize(out) / 1e6:.1f} MB")
        align_and_sign(out, tools, a.keystore, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    # Read it back out of the finished APK so the reported result is the real one.
    verify = read_cmdline(out)
    good(f"verified in output: {verify!r}")
    print()
    print(f"  {out}")
    print("  Re-signed, so uninstall the existing build on the headset first:")
    print("      adb uninstall com.AnotherAxiom.A2")
    print(f"      adb install \"{out}\"")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
