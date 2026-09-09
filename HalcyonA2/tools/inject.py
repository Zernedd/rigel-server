#!/usr/bin/env python3
"""
inject.py - minimal x64 DLL injector for Windows.

Same technique as tools/Inject.ps1 (which is what loads Dumper-7 into the game):
  OpenProcess -> VirtualAllocEx -> WriteProcessMemory(dll path, UTF-16)
  -> CreateRemoteThread(LoadLibraryW, remote path) -> wait -> VirtualFreeEx.

kernel32.dll is mapped at the same base in every process on a given boot, so the
LoadLibraryW address resolved here is valid inside the target.

Usage:
  python inject.py --pid 1234 --dll build\\x64\\Release\\HalcyonA2.dll
  python inject.py --process A2-Win64-Shipping.exe --dll tools\\Dumper7\\Dumper-7.dll
  python inject.py --process A2-Win64-Shipping --dll ...  --wait 60   # wait for it to appear
  python inject.py --list A2                                         # find a pid

Notes:
  * 64-bit Python only (the target is x64; a 32-bit host cannot inject into it).
  * Run elevated if the target runs elevated, otherwise OpenProcess fails with 5.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import sys
import time
from ctypes import wintypes

# --- Win32 -----------------------------------------------------------------------------

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT_RESERVE = 0x3000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04
INFINITE = 0xFFFFFFFF
WAIT_OBJECT_0 = 0x0
TH32CS_SNAPPROCESS = 0x2
MAX_PATH = 260

LPVOID = ctypes.c_void_p
SIZE_T = ctypes.c_size_t

kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.OpenProcess.restype = wintypes.HANDLE

kernel32.VirtualAllocEx.argtypes = [wintypes.HANDLE, LPVOID, SIZE_T, wintypes.DWORD, wintypes.DWORD]
kernel32.VirtualAllocEx.restype = LPVOID

kernel32.VirtualFreeEx.argtypes = [wintypes.HANDLE, LPVOID, SIZE_T, wintypes.DWORD]
kernel32.VirtualFreeEx.restype = wintypes.BOOL

kernel32.WriteProcessMemory.argtypes = [wintypes.HANDLE, LPVOID, LPVOID, SIZE_T, ctypes.POINTER(SIZE_T)]
kernel32.WriteProcessMemory.restype = wintypes.BOOL

kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
kernel32.GetModuleHandleW.restype = wintypes.HMODULE

kernel32.GetProcAddress.argtypes = [wintypes.HMODULE, wintypes.LPCSTR]
kernel32.GetProcAddress.restype = LPVOID

kernel32.CreateRemoteThread.argtypes = [
    wintypes.HANDLE, LPVOID, SIZE_T, LPVOID, LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)
]
kernel32.CreateRemoteThread.restype = wintypes.HANDLE

kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
kernel32.WaitForSingleObject.restype = wintypes.DWORD

kernel32.GetExitCodeThread.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
kernel32.GetExitCodeThread.restype = wintypes.BOOL

kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL

kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * MAX_PATH),
    ]


kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32FirstW.restype = wintypes.BOOL
kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
kernel32.Process32NextW.restype = wintypes.BOOL


class InjectError(RuntimeError):
    pass


def _win_error(what: str) -> InjectError:
    code = ctypes.get_last_error()
    return InjectError(f"{what} failed (error {code}: {ctypes.FormatError(code).strip()})")


# --- process lookup --------------------------------------------------------------------

def list_processes(substring: str = "") -> list[tuple[int, str]]:
    """Every running process as (pid, exe name), optionally filtered case-insensitively."""
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == wintypes.HANDLE(-1).value:
        raise _win_error("CreateToolhelp32Snapshot")

    found: list[tuple[int, str]] = []
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        needle = substring.lower()
        while ok:
            name = entry.szExeFile
            if not needle or needle in name.lower():
                found.append((entry.th32ProcessID, name))
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)
    return found


def find_pid(name: str) -> int:
    """Resolve a process name (with or without .exe) to exactly one pid."""
    target = name.lower()
    if not target.endswith(".exe"):
        target += ".exe"

    matches = [(pid, exe) for pid, exe in list_processes() if exe.lower() == target]
    if not matches:
        raise InjectError(f"no running process named '{name}'")
    if len(matches) > 1:
        pids = ", ".join(str(pid) for pid, _ in matches)
        raise InjectError(f"{len(matches)} processes named '{name}' ({pids}); pass --pid")
    return matches[0][0]


def wait_for_process(name: str, timeout: float) -> int:
    """Poll until a process with this name shows up, then return its pid."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            return find_pid(name)
        except InjectError:
            if time.monotonic() >= deadline:
                raise InjectError(f"'{name}' did not start within {timeout:.0f}s")
            time.sleep(0.5)


# --- injection -------------------------------------------------------------------------

def inject(pid: int, dll_path: str, timeout_ms: int = 30000) -> int:
    """Load dll_path into process `pid`. Returns the low 32 bits of the remote HMODULE."""
    dll_path = os.path.abspath(dll_path)
    if not os.path.isfile(dll_path):
        raise InjectError(f"DLL not found: {dll_path}")
    if ctypes.sizeof(ctypes.c_void_p) != 8:
        raise InjectError("run this with 64-bit Python - a 32-bit host cannot inject into an x64 process")

    handle = kernel32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not handle:
        raise _win_error(f"OpenProcess({pid})")

    remote = None
    try:
        # The target reads the path itself, so it has to live in the target's address space.
        buf = ctypes.create_unicode_buffer(dll_path)
        size = ctypes.sizeof(buf)

        remote = kernel32.VirtualAllocEx(handle, None, size, MEM_COMMIT_RESERVE, PAGE_READWRITE)
        if not remote:
            raise _win_error("VirtualAllocEx")

        written = SIZE_T(0)
        if not kernel32.WriteProcessMemory(handle, remote, ctypes.byref(buf), size, ctypes.byref(written)):
            raise _win_error("WriteProcessMemory")

        k32 = kernel32.GetModuleHandleW("kernel32.dll")
        if not k32:
            raise _win_error("GetModuleHandleW(kernel32.dll)")
        load_library = kernel32.GetProcAddress(k32, b"LoadLibraryW")
        if not load_library:
            raise _win_error("GetProcAddress(LoadLibraryW)")

        thread = kernel32.CreateRemoteThread(handle, None, 0, load_library, remote, 0, None)
        if not thread:
            raise _win_error("CreateRemoteThread")

        try:
            if kernel32.WaitForSingleObject(thread, timeout_ms) != WAIT_OBJECT_0:
                raise InjectError(
                    f"LoadLibraryW did not return within {timeout_ms}ms "
                    "(a DllMain that blocks can cause this; the DLL may still be loading)"
                )
            exit_code = wintypes.DWORD(0)
            kernel32.GetExitCodeThread(thread, ctypes.byref(exit_code))
        finally:
            kernel32.CloseHandle(thread)

        if exit_code.value == 0:
            raise InjectError(
                "LoadLibraryW returned NULL - the DLL failed to load in the target "
                "(missing dependency, wrong architecture, or DllMain returned FALSE)"
            )
        return exit_code.value
    finally:
        if remote:
            kernel32.VirtualFreeEx(handle, remote, 0, MEM_RELEASE)
        kernel32.CloseHandle(handle)


# --- cli -------------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Inject a DLL into a running x64 Windows process.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Usage:", 1)[1] if "Usage:" in __doc__ else None,
    )
    target = parser.add_mutually_exclusive_group()
    target.add_argument("--pid", type=int, help="target process id")
    target.add_argument("--process", "-p", help="target process name, e.g. A2-Win64-Shipping.exe")
    parser.add_argument("--dll", "-d", help="path to the DLL to inject")
    parser.add_argument("--wait", type=float, default=0, metavar="SECONDS",
                        help="with --process: wait up to SECONDS for the process to appear")
    parser.add_argument("--delay", type=float, default=0, metavar="SECONDS",
                        help="sleep this long after finding the process before injecting "
                             "(give the engine time to initialise)")
    parser.add_argument("--timeout", type=int, default=30000, metavar="MS",
                        help="how long to wait for LoadLibraryW to return (default 30000)")
    parser.add_argument("--list", nargs="?", const="", metavar="FILTER",
                        help="list running processes (optionally filtered) and exit")
    args = parser.parse_args(argv)

    if args.list is not None:
        for pid, name in sorted(list_processes(args.list), key=lambda p: p[1].lower()):
            print(f"{pid:>8}  {name}")
        return 0

    if not args.dll:
        parser.error("--dll is required (or use --list)")
    if args.pid is None and not args.process:
        parser.error("pass --pid or --process")

    try:
        if args.pid is not None:
            pid = args.pid
        elif args.wait > 0:
            print(f"[inject] waiting up to {args.wait:.0f}s for '{args.process}'...")
            pid = wait_for_process(args.process, args.wait)
        else:
            pid = find_pid(args.process)

        if args.delay > 0:
            print(f"[inject] found pid {pid}; waiting {args.delay:.0f}s before injecting")
            time.sleep(args.delay)

        base = inject(pid, args.dll, args.timeout)
        print(f"[inject] loaded {os.path.basename(args.dll)} into pid {pid} (module base 0x{base:X})")
        return 0
    except InjectError as exc:
        print(f"[inject] error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
