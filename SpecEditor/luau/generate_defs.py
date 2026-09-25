#!/usr/bin/env python3
"""
generate_defs.py -- build the luau-lsp definition file for Rigel / Spec Editor scripts.

Reads the Dumper-7 C++ SDK of the game (build 22284) and writes
    SpecEditor/luau/types/rigel.d.luau

What ends up in the file
  * every class that derives (directly or indirectly) from ULuauBehavior  -> components
  * the actor / API classes the game's own scripts use (DiscEntity, VRPawn, Gamemode, ...)
  * every class or struct whose name appears as a type in SpecEditor/luau/game-scripts/*.luau
  * the blueprint-backed component types the game scripts use (GameStateManagerBlueprintComponent, ...)
  * the game structs those signatures pass around (GoalInfo, LuauPhysicsObjectData, ...)
  * a hand-written prelude: log, warn, LuauClock, Promise, Vector, Ref, Enum, ...

Naming rule (verified against the game's scripts):
  C++ UGolfCupComponent::SetCupVisibility(bool Show)  ->  cup:setCupVisibility(show)
  static AVRPawn::GetLocalPlayerIndex(UWorld*)        ->  VRPawn.getLocalPlayerIndex()
  TMulticastInlineDelegate<void(...)> OnGoToSleep     ->  obj.onGoToSleep.Listen(function(...) end)

Usage
  python generate_defs.py                 # uses the default SDK path in this repo
  python generate_defs.py --sdk <dir> --out <file>

Only the Python standard library is needed.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import OrderedDict
from dataclasses import dataclass, field

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_SDK = os.path.join(REPO, "HalcyonA2", "HalcyonA2", "gamesdk", "22284", "SDK")
DEFAULT_OUT = os.path.join(HERE, "types", "rigel.d.luau")
DEFAULT_INDEX = os.path.join(HERE, "types", "api_index.json")
GAME_SCRIPTS = os.path.join(HERE, "game-scripts")

ROOT_BEHAVIOR = "ULuauBehavior"

# Extra C++ classes the game scripts rely on, plus the Luau name they are exposed under.
# (A None name means "use the default prefix-stripping rule".)
EXTRA_CLASSES = OrderedDict([
    ("ADiscEntity", None),
    ("AVRPawn", None),
    ("UModuleStateLuaAPI", None),      # the `Gamemode` global
    ("USandboxStatsLuauAPI", "Quests"),  # Quests.event / Quests.eventNumber / Quests.complete
])

# Globals that are *instances* of a class (called with ':').
INSTANCE_GLOBALS = OrderedDict([
    ("Gamemode", "ModuleStateLuaAPI"),
])

# Script-side type names that are backed by a Blueprint actor rather than a C++ component.
# The mapping is inferred from the method names each game script calls on them.
BLUEPRINT_TYPES = OrderedDict([
    ("GameStateManagerBlueprintComponent", "ABP_GameStateManager_C"),
    ("DriftballControlPanelBlueprintComponent", "ABP_DriftballControlPanel_C"),
    ("TrapSpinnerComponent", "ABP_Trap_Spinner_C"),
    ("TrapRotatingPlatformComponent", "ABP_Trap_RotatingPlatform_C"),
    ("SlidingPlatformComponent", "ABP_SlidingPlatform_C"),
    ("DeathrunResetTeleporterComponent", "ABP_DeathrunResetTeleporter_C"),
    ("aa_se_BP_TeleporterComponent", "ALE_BP_Teleporter_C"),
    ("GolfTeeComponent", "ABP_GolfTee_C"),
])
# Blueprint members are merged from the blueprint class and its C++ parents up to (not including) these.
BP_STOP = {"AActor", "APawn", "UObject", "UActorComponent", "USceneComponent"}

# Game structs that always get a type (they are built or read by the game scripts).
ALWAYS_STRUCTS = ["FGoalInfo", "FLuauPhysicsObjectData", "FBallSpawnParameters", "FCellStyle",
                  "FLayoutSettings", "FLayoutGap", "FStyleOverRange"]
# Only structs from these SDK packages are turned into types automatically.
STRUCT_PACKAGES = ("A2_structs.hpp", "SandboxEngine_structs.hpp", "Luau_structs.hpp",
                   "AAProgression_structs.hpp")

# Hand-written signatures that replace what the C++ header says (the Luau bridge differs here).
# key: (LuauClassName, luauMemberName) -> full member line (without leading indent)
OVERRIDES = {
    ("BallSpawnerComponent", "spawnBallWithParameters"):
        "function spawnBallWithParameters(self, parameters: BallSpawnParameters): Promise<DiscEntity>",
}
# Static overrides: (GlobalName, member) -> "name: type"
STATIC_OVERRIDES = {
    ("LuauClock", "timeout"): "timeout: (seconds: number) -> Promise<()>",
    ("LuauClock", "getTime"): "getTime: () -> number",
    ("LuauClock", "createTimer"): "createTimer: (betweenTime: number, callback: () -> ()) -> number",
}

# Classes whose Luau binding exposes only a FEW of their UFunctions. For these, the definitions list only members a
# runtime probe proved exist (every other declared member fails with "Unknown (Static) Property or Function").
# VRPawn, 2026-09-25 (mock player, tests/mcp_full_test.py + a member-by-member probe): 6 of 218 instance members and
# 5 of 10 statics are bound -- exactly what the station's own scripts use.
RUNTIME_VERIFIED = {
    "VRPawn": {
        "members": {"client_EmitStatEvent", "client_SetQuestCompleted", "emitStatEventFromLocalClient",
                    "setQuestCompletedFromLocalClient", "onBeforeDestroy", "OnBeforeDestroy"},
        "statics": {"getLocalPlayerIndex", "getPlayerByID", "getPlayerName", "getPositionByID", "getTeamIndexByID"},
    },
}


# Members a runtime probe proved are NOT bound, for classes whose binding covers most of what they declare
# (2026-09-25: every documented component wired into one script's slots, each declared member indexed). The rest
# of each class was proved to exist.
RUNTIME_MISSING = {
    "LuauBehavior": {"isNetworkReady"},
    "BasicButtonComponent": {"setButtonText", "setOnCooldown"},
    "TimerComponent": {"bTextIncludeMilliseconds", "timerTextArrayToUpdate"},
    "ToggleableComponent": {"defaultEnabledValue"},
    "PhysicalComponent": {"bClientsideOverride", "bIsVisibleClientside", "onHit", "overlapBegin", "overlapEnd"},
    "DataTableComponent": {"getTest", "hostTable", "tablesToCopyToo"},
    "BallSpawnerComponent": {"bBallWasGrounded", "bIsRunningGamemodeTests", "bShouldFreezeBallUponReset",
                             "bShouldSpawnBallFrozen", "bSmoothlyOffsetting", "bSmoothlyResetting", "bWaitingForBallNetGUID",
                             "checkForDeletedBalls", "checkIfBallGroundedUponHit", "preOffsetPosition", "preResetPosition",
                             "processQueuedGrounding", "resetHeartBall_Server", "smoothOffsetDestination",
                             "smoothOffsetStartTime", "smoothResetStartTime", "spawnHeartBall", "spawnedBalls"},
    "ScoreComponent": {"bCombineScores", "bouncesPlayerScored", "fastestGoalSpeed", "fastestPlayerScored", "lastBounces",
                       "lastGoalDistance", "lastGoalLocation", "lastGoalSpeed", "lastPasses", "lastPlayerScored",
                       "lastPlayerScoredID", "lastPlayerScoredTeam", "lastPoints", "longestGoalDistance",
                       "longestPlayerScored", "mostBounces", "mostPasses", "passesPlayerScored", "scoringTeam",
                       "teamCount", "teamPoints"},
    "DiscEntity": {"PlayerHit", "playerHit", "bHasBallBegunShrinking", "bIsFrozen", "bIsHeartBall", "bIsPersonalBall",
                   "ballStolen", "basePushbackPercentage", "clientUnreliablePlayerHit", "client_SetVisibilityOfDiscComponents",
                   "forceUpdate", "getCosmeticLoadout", "getDiscBounces", "getDiscPasses", "getGameSoundManagerEvent",
                   "getPlayerWhoSpawnedThisBall", "getTrailVisibility", "interceptionEvent", "multicast_UnreliablePlayerHit",
                   "networkedReleasePosition", "newHitEvent", "onGrabbedCallback", "onReleasedCallback",
                   "passEventServer", "playerHitOnServerLuau", "resetHeartBall", "resetMainBodyColor",
                   "serverDiscGrabbed", "serverDiscReleased", "setIgnorePawn", "setMainBodyColor",
                   "setPlayerWhoSpawnedThisBall", "setTrailVisibility", "spawner", "spawnerIndex",
                   "timeLastUsedByPoolingManager"},
}


def verified_ok(lname: str, member: str, static: bool = False) -> bool:
    if member in RUNTIME_MISSING.get(lname, ()):
        return False
    v = RUNTIME_VERIFIED.get(lname)
    return v is None or member in v["statics" if static else "members"]


# Globals written by hand in the prelude; generated tables must never reuse these names.
PRELUDE_GLOBALS = {"log", "warn", "Logging", "LuauClock", "Promise", "Vector", "Vec3", "LinearColor",
                   "Enum", "BallSpawnParameters", "GoalInfo", "CellStyle", "LayoutSettings",
                   "LayoutGap", "StyleOverRange", "Gamemode", "require", "math", "table",
                   "string", "coroutine", "task", "bit32", "utf8", "os", "debug", "buffer"}
PRELUDE_TYPES = {"Vector", "LinearColor", "Ref", "int", "Event", "Promise", "AnyPromise",
                 "LuauCallable"}

LUAU_KEYWORDS = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if",
                 "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until",
                 "while", "continue", "export", "type", "typeof", "self"}

# UFunctions that are Blueprint/engine plumbing, never meant to be called from a script.
SKIP_FUNC = re.compile(r"^(BndEvt__|ExecuteUbergraph|OnRep_|UserConstructionScript$|ReceiveBeginPlay$|"
                       r"ReceiveEndPlay$|ReceiveTick$|Timeline|CustomEvent|StaticClass$|StaticName$|"
                       r"GetDefaultObj$|BindLuauClasses$)|__")

NUMBER_TYPES = {"int8", "int16", "int32", "int64", "uint8", "uint16", "uint32", "uint64", "float",
                "double", "int", "unsigned int", "long", "short", "char"}
STRING_TYPES = {"FString", "FName", "FText"}
VECTOR_TYPES = {"FVector", "FRotator", "FVector_NetQuantize", "FVector_NetQuantize10",
                "FVector_NetQuantize100", "FVector_NetQuantizeNormal", "FVector3d", "FVector3f"}


# --------------------------------------------------------------------------------------------
# SDK parsing
# --------------------------------------------------------------------------------------------
@dataclass
class Param:
    name: str
    ctype: str      # bare type, e.g. "ADiscEntity", "TArray<FGoalInfo>", "int32"
    ptr: str        # "", "*", "**", "&"
    const: bool

    @property
    def is_out(self) -> bool:
        if self.ptr == "**":
            return True
        if self.ptr == "&" and not self.const:
            return True
        if self.ptr == "*" and not is_object_type(self.ctype):
            return True
        return False


@dataclass
class Func:
    name: str
    ret: str          # bare return type ("void" for none)
    ret_ptr: str
    params: list
    static: bool
    decl: str         # original C++ declaration


@dataclass
class Prop:
    name: str
    ctype: str
    ptr: str
    flags: str


@dataclass
class Delegate:
    name: str
    params: list      # list[Param] or None for sparse delegates (unknown signature)
    flags: str


@dataclass
class CppClass:
    name: str
    parent: str | None
    package: str
    funcs: list = field(default_factory=list)
    props: list = field(default_factory=list)
    delegates: list = field(default_factory=list)


@dataclass
class CppStruct:
    name: str
    package: str
    fields: list = field(default_factory=list)   # list[Prop]


CLASS_RE = re.compile(r"^class\s+(?:SDK_ALIGN\([^)]*\)\s+|alignas\([^)]*\)\s+)?(\w+)(?:\s+final)?\s*"
                      r"(?::\s*public\s+(\w+))?")
STRUCT_RE = re.compile(r"^struct\s+(?:SDK_ALIGN\([^)]*\)\s+|alignas\([^)]*\)\s+)?(F\w+)(?:\s+final)?\s*"
                       r"(?::\s*public\s+(\w+))?\s*$")
PROP_RE = re.compile(r"^\s+(.+?)\s+(\w+)(\[[^\]]*\])?;\s*//\s*0x[0-9A-Fa-f]+\(0x[0-9A-Fa-f]+\)\((.*)\)")
FUNC_RE = re.compile(r"^\s+(static\s+)?(.+?)\s+(\w+)\((.*)\)\s*(const)?\s*;\s*$")


def is_object_type(t: str) -> bool:
    """True when a bare C++ type name looks like a UObject/AActor/interface class."""
    return bool(re.match(r"^[UAI][A-Z0-9]\w*$", t)) or t in ("UObject", "AActor")


def strip_kw(t: str) -> str:
    t = re.sub(r"\b(class|struct|enum class|enum)\s+", "", t)
    return t.strip()


def split_top(s: str, sep: str = ",") -> list:
    parts, depth, cur = [], 0, []
    for ch in s:
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth -= 1
        if ch == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    if "".join(cur).strip():
        parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def parse_param(text: str) -> Param | None:
    text = text.strip()
    if not text or text == "void":
        return None
    const = False
    if text.startswith("const "):
        const = True
        text = text[6:].strip()
    m = re.match(r"^(.*?)([\*&]*)\s*(\w+)$", text)
    if not m:
        return None
    ctype, ptr, name = m.group(1).strip(), m.group(2), m.group(3)
    # "class ADiscEntity* Disc" -> regex puts '*' into ctype when there is no space; normalise
    while ctype.endswith("*") or ctype.endswith("&"):
        ptr = ctype[-1] + ptr
        ctype = ctype[:-1].strip()
    return Param(name=name, ctype=strip_kw(ctype), ptr=ptr, const=const)


def parse_delegate_sig(ctype: str):
    m = re.match(r"^TMulticastInlineDelegate<void\((.*)\)>$", ctype)
    if not m:
        return None
    params = []
    for i, p in enumerate(split_top(m.group(1))):
        pp = parse_param(p)
        if pp is None:
            pp = Param(name=f"arg{i + 1}", ctype="?", ptr="", const=True)
        params.append(pp)
    return params


def parse_sdk(sdk_dir: str):
    classes: dict[str, CppClass] = {}
    structs: dict[str, CppStruct] = {}
    enums: dict[str, list] = {}
    files = sorted(f for f in os.listdir(sdk_dir) if f.endswith("_classes.hpp") or f.endswith("_structs.hpp"))
    for fname in files:
        with open(os.path.join(sdk_dir, fname), encoding="utf-8-sig", errors="replace") as fh:
            lines = fh.read().splitlines()
        i = 0
        n = len(lines)
        while i < n:
            line = lines[i]
            cm = CLASS_RE.match(line) if fname.endswith("_classes.hpp") else None
            sm = STRUCT_RE.match(line) if fname.endswith("_structs.hpp") else None
            em = re.match(r"^enum class (E\w+)\s*:", line)
            if em:
                vals = []
                j = i + 2
                while j < n and not lines[j].startswith("};"):
                    vm = re.match(r"^\s+(\w+)\s*=\s*(-?\d+)", lines[j])
                    if vm and not vm.group(1).endswith("_MAX"):
                        vals.append((vm.group(1), int(vm.group(2))))
                    j += 1
                enums.setdefault(em.group(1), vals)
                i = j
                continue
            if not (cm or sm) or line.rstrip().endswith(";"):
                i += 1
                continue
            # find body
            j = i + 1
            while j < n and not lines[j].startswith("{"):
                j += 1
            body_start = j + 1
            k = body_start
            while k < n and not lines[k].startswith("};"):
                k += 1
            body = lines[body_start:k]
            if cm:
                cls = CppClass(name=cm.group(1), parent=cm.group(2), package=fname)
                parse_class_body(cls, body)
                classes.setdefault(cls.name, cls)
            else:
                st = CppStruct(name=sm.group(1), package=fname)
                for bl in body:
                    pm = PROP_RE.match(bl)
                    if pm and not pm.group(3):
                        st.fields.append(Prop(name=pm.group(2), ctype=strip_kw(pm.group(1)).rstrip("*").strip(),
                                              ptr="*" if pm.group(1).strip().endswith("*") else "",
                                              flags=pm.group(4)))
                structs.setdefault(st.name, st)
            i = k + 1
    return classes, structs, enums


def parse_class_body(cls: CppClass, body: list):
    brace_depth = 0
    for bl in body:
        if brace_depth > 0:
            brace_depth += bl.count("{") - bl.count("}")
            continue
        if bl.strip().startswith("{"):
            brace_depth += bl.count("{") - bl.count("}")
            continue
        pm = PROP_RE.match(bl)
        if pm:
            raw_type, name, arr, flags = pm.group(1).strip(), pm.group(2), pm.group(3), pm.group(4)
            if arr or name.startswith("Pad_") or name.startswith("UberGraphFrame"):
                continue
            if raw_type.startswith("TMulticastInlineDelegate"):
                sig = parse_delegate_sig(raw_type)
                if sig is not None:
                    cls.delegates.append(Delegate(name=name, params=sig, flags=flags))
                continue
            if raw_type.startswith("FMulticastSparseDelegateProperty_") or raw_type.startswith("FMulticast"):
                cls.delegates.append(Delegate(name=name, params=None, flags=flags))
                continue
            t = strip_kw(raw_type)
            ptr = ""
            while t.endswith("*"):
                ptr += "*"
                t = t[:-1].strip()
            cls.props.append(Prop(name=name, ctype=t, ptr=ptr, flags=flags))
            continue
        fm = FUNC_RE.match(bl)
        if fm and "//" not in bl:
            static = bool(fm.group(1))
            ret_raw = fm.group(2).strip()
            if ret_raw in ("return", "else") or "=" in ret_raw:
                continue
            ret = strip_kw(ret_raw)
            ret_ptr = ""
            while ret.endswith("*") or ret.endswith("&"):
                ret_ptr += ret[-1]
                ret = ret[:-1].strip()
            if ret.startswith("const "):
                ret = ret[6:].strip()
            params = []
            ok = True
            for p in split_top(fm.group(4)):
                pp = parse_param(p)
                if pp is None:
                    ok = False
                    break
                params.append(pp)
            if not ok:
                continue
            cls.funcs.append(Func(name=fm.group(3), ret=ret, ret_ptr=ret_ptr, params=params,
                                  static=static, decl=bl.strip()))


# --------------------------------------------------------------------------------------------
# Luau naming + type mapping
# --------------------------------------------------------------------------------------------
def lower_camel(name: str) -> str:
    return name[:1].lower() + name[1:] if name else name


def luau_class_name(cpp: str) -> str:
    if re.match(r"^[UA][A-Z]", cpp):
        return cpp[1:]
    return cpp


def safe_ident(name: str, used: set) -> str:
    raw = re.sub(r"_\d+$", "", name)
    base = (raw.lower() if raw.isupper() else lower_camel(raw)) or "arg"
    if not re.match(r"^[A-Za-z_]\w*$", base):
        base = "arg"
    if base in LUAU_KEYWORDS:
        base = base + "_"
    out = base
    k = 2
    while out in used:
        out = f"{base}{k}"
        k += 1
    used.add(out)
    return out


class TypeMapper:
    def __init__(self, class_names: dict, struct_names: dict):
        self.class_names = class_names    # cpp -> luau
        self.struct_names = struct_names  # cpp -> luau

    def map(self, t: str, ptr: str = "") -> str:
        t = t.strip()
        if t.startswith("const "):
            t = t[6:].strip()
        if t == "bool":
            return "boolean"
        if t in NUMBER_TYPES:
            return "number"
        if t in STRING_TYPES:
            return "string"
        if t in VECTOR_TYPES:
            return "Vector"
        if t == "FLinearColor":
            return "LinearColor"
        if t == "FLuaRef":
            return "Ref"
        if t == "FLuaCallable":
            return "LuauCallable"
        m = re.match(r"^TArray<(.*)>$", t)
        if m:
            inner = strip_kw(m.group(1))
            iptr = ""
            while inner.endswith("*"):
                iptr += "*"
                inner = inner[:-1].strip()
            return "{" + self.map(inner, iptr) + "}"
        if t in self.class_names:
            return self.class_names[t]
        if t in self.struct_names:
            return self.struct_names[t]
        return "any"


# --------------------------------------------------------------------------------------------
# Selection
# --------------------------------------------------------------------------------------------
def script_type_names() -> set:
    names = set()
    if not os.path.isdir(GAME_SCRIPTS):
        return names
    for f in os.listdir(GAME_SCRIPTS):
        if not f.endswith(".luau"):
            continue
        with open(os.path.join(GAME_SCRIPTS, f), encoding="utf-8", errors="replace") as fh:
            src = fh.read()
        # type annotations  "x: TypeName"  "{ TypeName }"  "[TypeName]:"
        for m in re.finditer(r":\s*\{?\s*([A-Za-z_]\w*)", src):
            names.add(m.group(1))
        for m in re.finditer(r"\{\s*([A-Z]\w*)\s*\}", src):
            names.add(m.group(1))
        # static-style globals  "TypeName.func("
        for m in re.finditer(r"\b([A-Z]\w*)\.\w+\s*\(", src):
            names.add(m.group(1))
    return names


def descends_from(classes, name, root) -> bool:
    seen = set()
    while name and name in classes and name not in seen:
        if name == root:
            return True
        seen.add(name)
        name = classes[name].parent
    return name == root


def luau_type_list(mapper, params):
    return [mapper.map(p.ctype, p.ptr) for p in params]


def fmt_pack(types: list) -> str:
    if not types:
        return "()"
    if len(types) == 1:
        return types[0]
    return "(" + ", ".join(types) + ")"


def event_type(mapper, params) -> str:
    if params is None:
        return "Event<...any>"
    types = []
    for p in params:
        types.append("any" if p.ctype == "?" else mapper.map(p.ctype, p.ptr))
    return "Event<(" + ", ".join(types) + ")>"


PRELUDE = r'''
-- =============================================================================================
--  rigel.d.luau  --  luau-lsp definition file for Rigel / Spec Editor Luau scripts
--  (Orion Drift build 22284)
--
--  GENERATED by SpecEditor/luau/generate_defs.py from the Dumper-7 SDK. Do not edit by hand;
--  change the generator and re-run it.
--
--  Method lists come from the game's class dump. Not every UFunction listed here is guaranteed
--  to be exposed to Luau or safe to call; test in a local server first.
-- =============================================================================================

-- ---------------------------------------------------------------------------------------------
-- Hand-written core types (from the game's own scripts)
-- ---------------------------------------------------------------------------------------------

-- The game scripts annotate integers as `int`.
export type int = number

-- Opaque Luau table handed to / from C++ (FLuaRef). EventComponent passes these around.
export type Ref = any

-- A Luau function handed to C++ (FLuaCallable).
export type LuauCallable = (...any) -> ...any

-- A multicast delegate exposed to Luau. The game scripts call it both ways:
--     obj.onSomething.Listen(function(...) end)     -- dot form (most common)
--     obj.onSomething:Listen(function(...) end)     -- colon form (also seen)
export type Event<T...> = {
	Listen: ((callback: (T...) -> ()) -> ()) & ((self: any, callback: (T...) -> ()) -> ()),
	listen: ((callback: (T...) -> ()) -> ()) & ((self: any, callback: (T...) -> ()) -> ()),
}

-- Result of chaining a promise (the chained value is not tracked precisely).
export type AnyPromise = {
	andThen: (self: AnyPromise, onResolve: (...any) -> ...any, onReject: ((...any) -> ...any)?) -> AnyPromise,
	await: (self: AnyPromise) -> ...any,
	expect: (self: AnyPromise) -> ...any,
}

-- Promise returned by LuauClock.timeout, Promise.new, spawnBallWithParameters, ...
export type Promise<T...> = {
	andThen: (self: Promise<T...>, onResolve: (T...) -> ...any, onReject: ((...any) -> ...any)?) -> AnyPromise,
	-- Yields the current coroutine until the promise settles. Use inside Promise.new(function() ... end).
	await: (self: Promise<T...>) -> T...,
	-- Like await, but raises an error if the promise was rejected.
	expect: (self: Promise<T...>) -> T...,
}

declare Promise: {
	new: <T...>(executor: (resolve: (T...) -> (), reject: (...any) -> ()) -> ()) -> Promise<T...>,
	resolve: <T...>(T...) -> Promise<T...>,
	try: <T...>(fn: () -> T...) -> Promise<T...>,
	race: (promises: { any }) -> AnyPromise,
}

-- 3D vector (Unreal units, cm). Supports + - * / with vectors and numbers.
declare extern type Vector with
	x: number
	y: number
	z: number
	function __add(self, other: Vector): Vector
	function __sub(self, other: Vector): Vector
	function __mul(self, other: Vector | number): Vector
	function __div(self, other: Vector | number): Vector
	function __unm(self): Vector
end

declare Vector: {
	new: (x: number, y: number, z: number) -> Vector,
}

-- Older alias used by TKB_Prime's gamemode script.
declare Vec3: {
	new: (x: number, y: number, z: number) -> Vector,
}

declare extern type LinearColor with
	r: number
	g: number
	b: number
	a: number
end

declare LinearColor: {
	new: (r: number, g: number, b: number, a: number) -> LinearColor,
}

-- Prints to the game log as  "LogLuau: Display: [<Script>.luau]: <message>"
declare function log(...: any): ()
declare function warn(...: any): ()

declare Logging: {
	warn: (category: string, ...any) -> (),
}

-- Rigel game modes (Spec Editor > Details > Game Modes). Works in any script inside a game mode's area, on every
-- machine; use it from BeginPlay or later (not at the top of the file). See Rigel-GameModes-Guide.pdf.
declare Rigel: {
	isGameMode: () -> boolean,
	name: () -> string,
	state: () -> string,            -- "idle" | "countdown" | "running" | "ended" | "none"
	isRunning: () -> boolean,
	round: () -> number,
	timeLeft: () -> number,         -- seconds left in the countdown / round (0 = no limit)
	teams: () -> number,
	teamName: (team: number) -> string,
	teamSize: (team: number) -> number,
	teamMax: (team: number) -> number,
	score: (team: number) -> number,
	roundsWon: (team: number) -> number,
	players: () -> number,
	winner: () -> number,           -- 0 = draw / none
	setting: (key: string) -> string,
	settingNumber: (key: string) -> number,
	startRound: () -> (),
	ringStart: () -> (),                -- like startRound, but ignored for the first 3 s of a lobby (start rings)
	startNow: () -> (),
	endRound: (winner: number?) -> (),
	resetGame: () -> (),
	resetBalls: () -> (),               -- the server spawns / resets every "Ball spawner" role ball
	addScore: (team: number, amount: number?) -> (),
	goal: (team: number, amount: number?, key: string?) -> (),   -- a goal: counted once per key per 2.5 s, then balls reset
	setScore: (team: number, value: number) -> (),
	setCustom: (key: string, value: string) -> (),
	setText: (name: string, value: string) -> (),
	onStateChanged: (f: (state: string, old: string) -> ()) -> (),
	onTeamChanged: (f: (team: number, size: number, old: number) -> ()) -> (),
	onScoreChanged: (f: (team: number, score: number, old: number) -> ()) -> (),
	onTimeChanged: (f: (secondsLeft: number) -> ()) -> (),
}

declare Enum: {
	TextJustify: {
		Left: number,
		Center: number,
		Right: number,
		InvariantLeft: number,
		InvariantRight: number,
	},
}

'''

STRUCT_CTORS = {
    # luau struct name -> constructor signature (as seen in the game scripts)
    "BallSpawnParameters": "new: (fields: { position: Vector?, isPersonalBall: boolean?, targetPlayerIndex: number?, type: string? }) -> BallSpawnParameters",
    "GoalInfo": "new: (fields: { [string]: any }) -> GoalInfo",
    "CellStyle": "new: (fields: { [string]: any }) -> CellStyle",
    "LayoutSettings": "new: (fields: { [string]: any }) -> LayoutSettings",
    "LayoutGap": "new: (targetIndex: number, gap: number) -> LayoutGap",
    "StyleOverRange": "new: (style: CellStyle, startRow: number, startCol: number, endRow: number, endCol: number) -> StyleOverRange",
}


def generate(sdk_dir: str, out_path: str, index_path: str | None):
    classes, structs, enums = parse_sdk(sdk_dir)
    if ROOT_BEHAVIOR not in classes:
        sys.exit(f"could not find {ROOT_BEHAVIOR} in {sdk_dir}")

    # ---- choose classes -------------------------------------------------------------------
    chosen: "OrderedDict[str, str]" = OrderedDict()   # cpp -> luau name
    reasons: dict[str, str] = {}

    def choose(cpp, luau=None, why=""):
        if cpp in chosen or cpp not in classes:
            return
        name = luau or luau_class_name(cpp)
        if name in chosen.values() or name in PRELUDE_TYPES:
            return
        chosen[cpp] = name
        reasons[cpp] = why

    for cpp in sorted(classes):
        if descends_from(classes, cpp, ROOT_BEHAVIOR):
            choose(cpp, why="LuauBehavior component")
    for cpp, luau in EXTRA_CLASSES.items():
        choose(cpp, luau, why="used by the game scripts")
    wanted = script_type_names()
    for w in sorted(wanted):
        for cand in ("U" + w, "A" + w):
            if cand in classes:
                choose(cand, why="named in a game script")
    for luau, cpp in BLUEPRINT_TYPES.items():
        choose(cpp, luau, why="blueprint-backed component used by a game script")

    # ---- choose structs ---------------------------------------------------------------------
    struct_names: "OrderedDict[str, str]" = OrderedDict()
    for s in ALWAYS_STRUCTS:
        if s in structs:
            struct_names[s] = s[1:]
    for w in sorted(wanted):
        if "F" + w in structs and structs["F" + w].package in STRUCT_PACKAGES:
            struct_names["F" + w] = w

    def note_struct(t):
        t = strip_kw(t)
        m = re.match(r"^TArray<(.*)>$", t)
        if m:
            return note_struct(m.group(1).rstrip("*").strip())
        if t in structs and structs[t].package in STRUCT_PACKAGES and t not in struct_names \
                and t not in ("FLuaRef", "FLuaCallable") and t[1:] not in PRELUDE_TYPES \
                and t[1:] not in chosen.values():
            struct_names[t] = t[1:]

    for cpp in chosen:
        c = classes[cpp]
        for f in c.funcs:
            note_struct(f.ret)
            for p in f.params:
                note_struct(p.ctype)
        for d in c.delegates:
            for p in (d.params or []):
                note_struct(p.ctype)
    # one level of nesting for struct fields
    for s in list(struct_names):
        for fld in structs[s].fields:
            note_struct(fld.ctype)

    mapper = TypeMapper(dict(chosen), dict(struct_names))
    is_bp = {cpp for cpp in chosen if cpp.endswith("_C")}

    # ---- order classes so parents come first ---------------------------------------------
    def chosen_parent(cpp):
        p = classes[cpp].parent
        seen = set()
        while p and p not in seen:
            if p in chosen:
                return p
            seen.add(p)
            p = classes[p].parent if p in classes else None
        return None

    ordered, visiting = [], set()

    def visit(cpp):
        if cpp in ordered or cpp in visiting:
            return
        visiting.add(cpp)
        par = chosen_parent(cpp)
        if par:
            visit(par)
        ordered.append(cpp)

    for cpp in chosen:
        visit(cpp)

    # ---- emit -----------------------------------------------------------------------------
    out = [PRELUDE]
    index = {"classes": OrderedDict(), "globals": OrderedDict()}
    stats = {"classes": 0, "methods": 0, "events": 0, "properties": 0, "static": 0, "structs": 0}
    member_names: dict[str, set] = {}
    statics_by_global: "OrderedDict[str, list]" = OrderedDict()

    out.append("-- ---------------------------------------------------------------------------------------------\n")
    out.append("-- Game structs (field names are lowerCamel, e.g. goalInfo.scoringTeam)\n")
    out.append("-- ---------------------------------------------------------------------------------------------\n\n")
    for cpp_s, luau_s in struct_names.items():
        st = structs[cpp_s]
        out.append(f"-- {cpp_s} ({st.package.replace('_structs.hpp', '')})\n")
        out.append(f"declare extern type {luau_s} with\n")
        seen = set()
        for fld in st.fields:
            if fld.name.startswith("Pad_") or fld.name.startswith("Set_"):
                continue
            fname = lower_camel(fld.name)
            if fname in seen or fname in LUAU_KEYWORDS or not re.match(r"^[A-Za-z_]\w*$", fname):
                continue
            seen.add(fname)
            out.append(f"\t{fname}: {mapper.map(fld.ctype, fld.ptr)}\n")
        out.append("end\n\n")
        stats["structs"] += 1

    out.append("-- ---------------------------------------------------------------------------------------------\n")
    out.append("-- Components (ULuauBehavior subclasses), actors and APIs\n")
    out.append("-- ---------------------------------------------------------------------------------------------\n\n")

    for cpp in ordered:
        c = classes[cpp]
        lname = chosen[cpp]
        par = chosen_parent(cpp)
        inherited = set()
        pp = par
        while pp:
            inherited |= member_names.get(pp, set())
            pp = chosen_parent(pp)
        names: set = set()
        body = []
        entry = {"cpp": cpp, "package": c.package.replace("_classes.hpp", ""), "parent": chosen.get(par) if par else None,
                 "why": reasons.get(cpp, ""), "methods": [], "events": [], "properties": [], "static": []}

        def add_name(n):
            if n in names or n in inherited or n in LUAU_KEYWORDS or not re.match(r"^[A-Za-z_]\w*$", n):
                return False
            names.add(n)
            return True

        bp = cpp in is_bp
        funcs, delegates, props = list(c.funcs), list(c.delegates), list(c.props)
        if bp:
            # a blueprint's callable surface includes its native C++ parents (e.g. AGolfTee)
            anc = c.parent
            while anc and anc in classes and anc not in BP_STOP and anc not in chosen:
                funcs += classes[anc].funcs
                delegates += classes[anc].delegates
                props += classes[anc].props
                anc = classes[anc].parent
        # methods
        for f in sorted(funcs, key=lambda f: f.name.lower()):
            if SKIP_FUNC.search(f.name):
                continue
            if f.static:
                continue
            luau_names = [lower_camel(f.name)]
            if bp:
                # blueprint functions are called both as written (PlaySaveEvent) and in lowerCamel;
                # names with underscores are exposed by their display name (Set_Enable_Arrow -> setEnableArrow)
                if "_" in f.name.strip("_"):
                    luau_names.append(lower_camel(f.name.replace("_", "")))
                luau_names.append(f.name)
                luau_names = list(dict.fromkeys(luau_names))
            for ln in luau_names:
                if not verified_ok(lname, ln) or not add_name(ln):
                    continue
                ov = OVERRIDES.get((lname, ln))
                if ov:
                    body.append(f"\t-- C++: {f.decl}\n\t{ov}\n")
                    entry["methods"].append({"name": ln, "sig": ov.replace("function ", "", 1), "cpp": f.decl})
                    stats["methods"] += 1
                    continue
                used = set()
                ins, outs = [], []
                for p in f.params:
                    if p.is_out:
                        outs.append(mapper.map(p.ctype, p.ptr))
                    else:
                        ins.append(f"{safe_ident(p.name, used)}: {mapper.map(p.ctype, p.ptr)}")
                rets = ([] if f.ret == "void" else [mapper.map(f.ret, f.ret_ptr)]) + outs
                sig = f"({', '.join(['self'] + ins)}): {fmt_pack(rets)}"
                body.append(f"\t-- C++: {f.decl}\n\tfunction {ln}{sig}\n")
                entry["methods"].append({"name": ln, "sig": f"{ln}{sig}", "cpp": f.decl})
                stats["methods"] += 1
        # events (delegates) -- both lowerCamel and the original C++ name
        for d in delegates:
            if not verified_ok(lname, lower_camel(d.name)) and not verified_ok(lname, d.name):
                continue
            et = event_type(mapper, d.params)
            for ln in dict.fromkeys([lower_camel(d.name), d.name]):
                if add_name(ln):
                    body.append(f"\t{ln}: {et}\n")
            entry["events"].append({"name": lower_camel(d.name), "type": et,
                                    "args": [f"{p.name}: {('any' if p.ctype == '?' else mapper.map(p.ctype, p.ptr))}"
                                             for p in (d.params or [])] if d.params is not None else None})
            stats["events"] += 1
        # simple public properties
        for p in props:
            if "NativeAccessSpecifierPublic" not in p.flags and not bp:
                continue
            t = mapper.map(p.ctype, p.ptr)
            if t == "any":
                continue
            ln = lower_camel(p.name)
            if verified_ok(lname, ln) and add_name(ln):
                body.append(f"\t{ln}: {t}\n")
                entry["properties"].append({"name": ln, "type": t})
                stats["properties"] += 1
        # statics -> global table
        for f in sorted(c.funcs, key=lambda f: f.name.lower()):
            if not f.static or SKIP_FUNC.search(f.name) or f.name.startswith("LUA_")                     or f.name == "BindLuauClasses":
                continue
            params = list(f.params)
            if params and params[0].ctype in ("UWorld", "UObject") and params[0].ptr == "*":
                params = params[1:]     # world context is supplied by the bridge
            used = set()
            ins, outs = [], []
            for p in params:
                if p.is_out:
                    outs.append(mapper.map(p.ctype, p.ptr))
                else:
                    ins.append(f"{safe_ident(p.name, used)}: {mapper.map(p.ctype, p.ptr)}")
            rets = ([] if f.ret == "void" else [mapper.map(f.ret, f.ret_ptr)]) + outs
            ln = lower_camel(f.name)
            if not verified_ok(lname, ln, static=True):
                continue
            ov = STATIC_OVERRIDES.get((lname, ln))
            line = ov or f"{ln}: ({', '.join(ins)}) -> {fmt_pack(rets)}"
            statics_by_global.setdefault(lname, []).append((line, f.decl))
            entry["static"].append({"name": ln, "sig": line, "cpp": f.decl})
            stats["static"] += 1

        member_names[cpp] = names
        header = f"-- {cpp} ({entry['package']})" + (" -- blueprint-backed" if bp else "") + "\n"
        ext = f" extends {chosen[par]}" if par else ""
        out.append(header)
        out.append(f"declare extern type {lname}{ext} with\n")
        out.extend(body)
        out.append("end\n\n")
        index["classes"][lname] = entry
        stats["classes"] += 1

    # LuauClock is an API object (not a component); make sure its statics exist even if the
    # class were ever dropped from the SDK.
    out.append("-- ---------------------------------------------------------------------------------------------\n")
    out.append("-- Static functions -> global tables  (e.g. VRPawn.getLocalPlayerIndex())\n")
    out.append("-- ---------------------------------------------------------------------------------------------\n\n")
    if "LuauClock" not in statics_by_global:
        statics_by_global["LuauClock"] = []
    have = {l.split(":")[0] for l, _ in statics_by_global["LuauClock"]}
    for (g, m), line in STATIC_OVERRIDES.items():
        if g == "LuauClock" and m not in have:
            statics_by_global["LuauClock"].append((line, "(hand-written)"))
    for g, members in statics_by_global.items():
        if g in PRELUDE_GLOBALS and g != "LuauClock":
            continue
        out.append(f"declare {g}: {{\n")
        seen = set()
        for line, decl in members:
            key = line.split(":")[0]
            if key in seen:
                continue
            seen.add(key)
            out.append(f"\t-- C++: {decl}\n\t{line},\n")
        out.append("}\n\n")
        index["globals"][g] = [l for l, _ in members]

    # struct constructors
    out.append("-- Struct constructors seen in the game scripts\n")
    for s, ctor in STRUCT_CTORS.items():
        if s in struct_names.values():
            out.append(f"declare {s}: {{\n\t{ctor},\n}}\n\n")
            index["globals"][s] = [ctor]

    # instance globals
    out.append("-- The gamemode this script belongs to (config variables, scores, quests, start/stop).\n")
    for g, t in INSTANCE_GLOBALS.items():
        if t in chosen.values():
            out.append(f"declare {g}: {t}\n")
            index["globals"][g] = [f"instance of {t}"]
    out.append("\n-- Lifecycle: the game calls these globals if the script defines them.\n")
    out.append("--   function BeginPlay() ... end   -- after External Dependencies are filled in\n")
    out.append("--   function EndPlay() ... end     -- when the object/gamemode is torn down\n")

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    text = "".join(out)
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    if index_path:
        index["stats"] = stats
        with open(index_path, "w", encoding="utf-8", newline="\n") as fh:
            json.dump(index, fh, indent=1)
    return stats


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sdk", default=DEFAULT_SDK, help="Dumper-7 SDK folder (contains *_classes.hpp)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output .d.luau path")
    ap.add_argument("--index", default=DEFAULT_INDEX, help="also write a JSON API index here ('' to skip)")
    a = ap.parse_args()
    stats = generate(a.sdk, a.out, a.index or None)
    print(f"wrote {a.out}")
    print("  classes: {classes}  methods: {methods}  events: {events}  properties: {properties}  "
          "static functions: {static}  structs: {structs}".format(**stats))


if __name__ == "__main__":
    main()
