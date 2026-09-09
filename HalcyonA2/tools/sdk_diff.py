"""Diff member offsets between two Dumper-7 dumps for the classes the payload hardcodes."""
import re, glob, os, sys
OLD="C:/Dumper-7/5.4.2-20996+++A2+Release-A2/CppSDK/SDK"
NEW="C:/Dumper-7/5.4.2-22284+++A2+Release-A2/CppSDK/SDK"
CLASSES=sys.argv[1:] or ["UA2VOIPSubsystem","AVRPawn","ABallSimManager","UOfflineBallSimSubsystem","UA2PlayerEntity",
        "UA2PhysicsSync","AActor","UWorld","ATicketManager","UGoalComponent","AA2GameModeBase","UNetEventsBridge",
        "APlayerState","UA2StationDashboardSubsystem","AGoal","UA2VOIPComponent"]
def load(root):
    out={}
    for f in glob.glob(root+"/*_classes.hpp")+glob.glob(root+"/*_structs.hpp"):
        txt=open(f,encoding="utf-8",errors="replace").read()
        for m in re.finditer(r'^(?:class|struct)\s+(?:alignas\(\w+\)\s+)?(\w+)\b[^\n]*\n\{\n(.*?)\n\};', txt, re.S|re.M):
            name, body = m.group(1), m.group(2)
            members={}
            for mm in re.finditer(r'^\s+(.+?)\s+(\w+)(?:\[[^\]]*\])?;\s*//\s*0x([0-9A-Fa-f]+)\(0x([0-9A-Fa-f]+)\)', body, re.M):
                members[mm.group(2)]=(int(mm.group(3),16), int(mm.group(4),16), mm.group(1).strip())
            size=re.search(r'// 0x[0-9A-Fa-f]+ \(0x([0-9A-Fa-f]+) - 0x[0-9A-Fa-f]+\)', txt[max(0,m.start()-200):m.start()])
            out[name]=(members, int(size.group(1),16) if size else None)
    return out
o,n=load(OLD),load(NEW)
for c in CLASSES:
    if c not in o and c not in n: print(f"== {c}: not found in either"); continue
    om,osz=o.get(c,({},None)); nm,nsz=n.get(c,({},None))
    print(f"== {c}  size old={osz and hex(osz)} new={nsz and hex(nsz)}")
    names=[k for k in om if not k.startswith("Pad_")]+[k for k in nm if k not in om and not k.startswith("Pad_")]
    for k in names:
        a=om.get(k); b=nm.get(k)
        if a and b and a[0]!=b[0]: print(f"   {k:<40} 0x{a[0]:04X} -> 0x{b[0]:04X}  ({b[0]-a[0]:+#x})")
        elif a and not b: print(f"   {k:<40} 0x{a[0]:04X} -> REMOVED")
        elif b and not a: print(f"   {k:<40}   new   -> 0x{b[0]:04X}  ({b[2]})")
