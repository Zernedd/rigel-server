#!/usr/bin/env python3
"""
patch_port_22284.py - apply the IDA-verified corrections to the provided 22284 port (HalcyonA2/dllmain.cpp).

Every replacement is exact-match and must occur exactly once, so re-running on an already patched
file fails loudly instead of double-applying. A backup dllmain.cpp.user-port.bak is written first.
"""
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
P = os.path.join(HERE, "..", "HalcyonA2", "dllmain.cpp")
BAK = P + ".user-port.bak"
if not os.path.exists(BAK):
    shutil.copyfile(P, BAK)
s = open(P, encoding="utf-8", errors="surrogateescape").read()
if "[PORT-AUDIT]" in s:
    print("already patched"); sys.exit(0)


def rep(old, new):
    global s
    n = s.count(old)
    assert n == 1, (old[:80], n)
    s = s.replace(old, new)


# 1. verified RVAs -----------------------------------------------------------------------
rep("static constexpr uintptr_t SendResults_RVA = 0x54E35B0;",
    "static constexpr uintptr_t SendResults_RVA = 0x534A7F0;   // [PORT-AUDIT] was 0x54E35B0 (= the pawn-side results RECEIVER); 0x534A7F0 is the sender called from StepSim (IDA-aligned 1.00 to 20996 0x5309C40)")
rep("static constexpr uintptr_t GolfBallInCup_RVA = 0x53DC340;",
    "static constexpr uintptr_t GolfBallInCup_RVA = 0x53DC320;   // [PORT-AUDIT] was 0x53DC340 (3-arg inner fn, hooked with a 2-arg thunk); 0x53DC320 = 20996 0x53986A0 equivalent")
rep("static constexpr uintptr_t GolfOverlap_RVA = 0x53F0410;",
    "static constexpr uintptr_t GolfOverlap_RVA = 0x53F03B0;   // [PORT-AUDIT] was 0x53F0410 (unrelated 10-byte fn); 0x53F03B0 = 20996 0x53AD660 equivalent (align 1.00)")
rep("static constexpr uintptr_t FNameResolve_RVA = 0x114E210;",
    "static constexpr uintptr_t FNameResolve_RVA = 0x114D690;   // [PORT-AUDIT] 22284 (align 1.00 to 20996 0x114E210)")
rep("static constexpr uintptr_t TextLayoutLeaf_RVA = 0x1EC13E0;",
    "static constexpr uintptr_t TextLayoutLeaf_RVA = 0x1EC07A0;   // [PORT-AUDIT] 22284 (align 1.00 to 20996 0x1EC13E0)")
rep("static constexpr uintptr_t EvtDispatch_RVA = 0x465F820;",
    "static constexpr uintptr_t EvtDispatch_RVA = 0x466A240;   // [PORT-AUDIT] 22284 (align 1.00 to 20996 0x465F820); hook still not installed (diagnostic only)")

# 2. RawData netvar vtable ------------------------------------------------------------------
rep("            if (vt == GetBase() + 0x7FE6540)                                   // RawData netvar vtable",
    "            if (vt == GetBase() + 0x804BDA0)                                   // RawData netvar vtable [PORT-AUDIT] 22284 (was 20996 0x7FE6540; mapped via 3 aligned referrers)")

# 3. NetVarReg: filter + two hooks (the function exists twice on 22284) -------------------------
rep("static __int64 __fastcall NetVarReg_Hook(void* netvar)\n{\n    int didPatch = 0;",
    "static void NetVarRegFilter(void* netvar)\n{\n    int didPatch = 0;")
rep('    if (didPatch) { ++g_lodNetvarPatched; printf("[HalcyonA2][LOD] rewrote DefaultLODSettings netvar -> no-hide (#%d)\\n", g_lodNetvarPatched); }\n    return g_NetVarReg_Orig(netvar);\n}',
    '    if (didPatch) { ++g_lodNetvarPatched; printf("[HalcyonA2][LOD] rewrote DefaultLODSettings netvar -> no-hide (#%d)\\n", g_lodNetvarPatched); }\n}\n'
    '// [PORT-AUDIT] 20996 sub_46A0DF0 exists as two identical copies on 22284: 0x46BC830 (called from the old-style callers)\n'
    '// and 0x47138D0 (called from the new ObjBuild). Hook both; the port never installed this hook at all.\n'
    'static NetVarReg_t g_NetVarReg_Orig2 = nullptr;\n'
    'static __int64 __fastcall NetVarReg_Hook(void* netvar)  { NetVarRegFilter(netvar); return g_NetVarReg_Orig(netvar); }\n'
    'static __int64 __fastcall NetVarReg_Hook2(void* netvar) { NetVarRegFilter(netvar); return g_NetVarReg_Orig2(netvar); }')

# 4. kick helper ----------------------------------------------------------------------------
rep("static bool g_gateEnforce = true;",
    '// [PORT-AUDIT] The 20996 kick helper (0x5333610 = pc->vtbl[77](pc, FText("Kick"), 0), reached from Server_KickPlayer) does not\n'
    '// exist on 22284 (0x5333610 is now +0x10 inside an unrelated 0x2F-byte fn; the Kick UFunctions are gone from the SDK).\n'
    '// Calling it corrupted the stack on the first enforced kick. Kick through the engine RPC instead: the client returns to\n'
    '// the main menu and drops the connection.\n'
    'static void KickPcViaRpc(uintptr_t pc)\n'
    '{\n'
    '    auto* p = reinterpret_cast<SDK::APlayerController*>(pc);\n'
    '    SDK::FString reason(L"Kicked by server");\n'
    '    p->ClientReturnToMainMenuWithTextReason(SDK::UKismetTextLibrary::Conv_StringToText(reason));\n'
    '}\n'
    'static void SafeKickPc(uintptr_t pc) { __try { KickPcViaRpc(pc); } __except (EXCEPTION_EXECUTE_HANDLER) { HxLog("[HalcyonA2][GATE] kick RPC faulted pc=%p\\n", (void*)pc); } }\n\n'
    'static bool g_gateEnforce = true;')
rep("            reinterpret_cast<void(__fastcall*)(void*)>(GetBase() + 0x5333610)(reinterpret_cast<void*>(pc));",
    "            SafeKickPc(pc);   // [PORT-AUDIT] was a raw call into 0x5333610 (mid-function on 22284)")

# 5. net-mode scanner range: exact 20996 parity ------------------------------------------------
rep("    const uintptr_t lo   = base + 0x5400000;   // whole ball-sim + physics-helper cluster\n    const uintptr_t hi   = base + 0x5500000;   // [22284] widened to cover StepSim 0x54863C0 / the moved cluster",
    "    // [PORT-AUDIT] 20996 range 0x5400000-0x5480000 patched exactly six sites (seater-binder, build worker, sub_5414850,\n"
    "    // DashboardInit, sub_547AE70, sub_547B3C0). Their 22284 equivalents are 0x545B446/0x545CD31/0x5461A1B/0x54B141A/\n"
    "    // 0x54B3351/0x54B358C, all inside [0x5450000,0x54B4000). The previous 0x5400000-0x5500000 ALSO patched six unrelated\n"
    "    // sites - five in the VOIP/OnlineCommunications region (0x542DAC0, 0x5436A00, 0x5436BC0, 0x54379E0, 0x54380E0: the\n"
    "    // ListenServer-only positional-participant list paths) and 0x54BA8C0 - which 20996 never touched.\n"
    "    const uintptr_t lo   = base + 0x5450000;\n    const uintptr_t hi   = base + 0x54B4000;")

# 6. entitlement self-exit guard for the server process -----------------------------------------
rep('    printf("[HalcyonA2] GIsClient=false, GIsServer=true (before open)\\n");',
    '    printf("[HalcyonA2] GIsClient=false, GIsServer=true (before open)\\n");\n\n'
    '    // [PORT-AUDIT] Entitlement self-exit guard for the SERVER process. When the Oculus entitlement check fails on the\n'
    '    // host (err log: "GetSignatureToken get_signature error: Missing entitlement"), LogA2MothershipAuthStateMachine\n'
    '    // requests an engine exit ("Could not verify entitlement status ... Closing by request") ~30s after boot and the\n'
    '    // process dies (exit code 3, crash in the object-teardown loop). Same patch as the A2EntitlementPatch UE4SS mod:\n'
    '    // flip the jne at 0x5429427 (20996: 0x53DFCC7) to jmp. Signature-guarded.\n'
    '    {\n'
    '        static const uint8_t entSig[] = { 0x40, 0x84, 0xED, 0x75, 0x59, 0x80, 0x3D };\n'
    '        const uint8_t* ep = reinterpret_cast<const uint8_t*>(base + 0x5429424);\n'
    '        if (memcmp(ep, entSig, sizeof(entSig)) == 0 && ep[11] == 0x02 && ep[12] == 0x72 && ep[13] == 0x25 && ep[14] == 0x83 && ep[15] == 0x7F)\n'
    '        {\n'
    '            WriteByte(base + 0x5429427, 0xEB);\n'
    '            printf("[HalcyonA2] entitlement self-exit patched (0x5429427 jne->jmp)\\n");\n'
    '            HxLog("[HalcyonA2] entitlement self-exit patched (0x5429427 jne->jmp)\\n");\n'
    '        }\n'
    '        else { printf("[HalcyonA2] WARNING: entitlement patch signature mismatch @0x5429424 - NOT patched\\n"); HxLog("[HalcyonA2] WARNING: entitlement patch signature mismatch @0x5429424 - NOT patched\\n"); }\n'
    '    }')

# 7. re-enable the crash guards + install NetVarReg ----------------------------------------------
rep("    // [PORT 22284] FNameResolve(0x114E210) + TextLayoutLeaf(0x1EC13E0) crash-guard hooks DISABLED —\n    // RVAs unfound (all-nameless neighbors). These are runtime crash-driven; add if/when they fault.",
    '    // [PORT-AUDIT] FNameResolve / TextLayoutLeaf crash guards re-enabled with the verified 22284 RVAs.\n'
    '    {\n'
    '        MH_STATUS sFN = MH_CreateHook(reinterpret_cast<void*>(base + FNameResolve_RVA), &FNameResolve_Hook, reinterpret_cast<void**>(&FNameResolve_Orig));\n'
    '        MH_STATUS sTL = MH_CreateHook(reinterpret_cast<void*>(base + TextLayoutLeaf_RVA), &TextLayoutLeaf_Hook, reinterpret_cast<void**>(&TextLayoutLeaf_Orig));\n'
    '        printf("[HalcyonA2] crash guards FNameResolve=%s TextLayoutLeaf=%s\\n", MH_StatusToString(sFN), MH_StatusToString(sTL));\n'
    '        MH_STATUS sNV1 = MH_CreateHook(reinterpret_cast<void*>(base + 0x46BC830), &NetVarReg_Hook,  reinterpret_cast<void**>(&g_NetVarReg_Orig));\n'
    '        MH_STATUS sNV2 = MH_CreateHook(reinterpret_cast<void*>(base + 0x47138D0), &NetVarReg_Hook2, reinterpret_cast<void**>(&g_NetVarReg_Orig2));\n'
    '        printf("[HalcyonA2] NetVarReg (LOD no-hide) hooks %s / %s\\n", MH_StatusToString(sNV1), MH_StatusToString(sNV2));\n'
    '    }')

# 8. empty phantom (-2) sim build skip, flag-controlled ------------------------------------------
rep("static bool       g_ballFilterPhantom = true;   // drop phantom-only sim builds (the GC-flood source)",
    "static bool       g_ballFilterPhantom = true;   // drop phantom-only sim builds (the GC-flood source)\n"
    "static bool       g_skipEmptyPhantomBuild = false;  // [PORT-AUDIT] OFF: tried 2026-09-06 - skipping the empty -2 build faults at 0x547AE4B (the tick derefs the missing -2 entry, RDX=-2, RAX=0)")
rep("            if (data && count > 0 && count < 4096)\n            {\n                // Every participant byte is a pawn PlayerIndex@0x1C22.",
    '            if (g_skipEmptyPhantomBuild && (int)simId < 0 && count == 0)\n'
    '            {\n'
    '                // [PORT-AUDIT] headless run 2026-09-06: the first native tick builds the local-player prediction sim\n'
    '                // ("[DiagBUILD] Index=-2 participants=0 []") holding EVERY ball ("[SIMPART] sim=-2 balls=51"). The\n'
    '                // phantom filter below only covered count>0, so this empty build always went through on 22284\'s\n'
    '                // restructured build worker (0x545CCB0, 0.06 alignment to 20996). Skip it like the phantom-only case.\n'
    '                static uint64_t s_lastE = 0; const uint64_t nowE = GetTickCount64();\n'
    '                if (nowE - s_lastE > 5000) { s_lastE = nowE; printf("[HalcyonA2][BALLFILTER] skipped EMPTY phantom sim build (simId=%d)\\n", (int)simId); }\n'
    '                return 0.0;\n'
    '            }\n'
    "            if (data && count > 0 && count < 4096)\n            {\n                // Every participant byte is a pawn PlayerIndex@0x1C22.")

open(P, "w", encoding="utf-8", errors="surrogateescape", newline="").write(s)
print("patched OK:", s.count("[PORT-AUDIT]"), "markers")
