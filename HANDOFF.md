# Rigel private server — handoff brief

Paste this as your first message in a new session **on the VPS**. It is written to be
self-contained: everything below is verified state, not guesswork.

---

## 1. What this is

A private server for **Orion Drift / A2, build 22284** (UE 5.4.2, Meta Quest).

Three moving parts:

| Part | What it is | Runs on |
|---|---|---|
| **Backend** | `.NET 6` — Mothership (login/auth) `:90`, StationDb (stations/roles/quests) `:78`, fake EOS gateway `:50` + WS `:80`, Dashboard `:8080`, orchestrator `:9095`/`:9100` | the VPS (Windows) |
| **Game server** | `A2-Win64-Shipping.exe` + injected `HalcyonA2.dll`. **Windows only** (WinHTTP, SEH/VEH). Players connect over **UDP 7777+** | the VPS (Windows) |
| **Client** | "Rigel" = our own Quest build, package `org.zern.rigel`, distributed via **Meta App Lab (ALPHA channel)** | Quest headset |

**Both backend and game server now run on the same Windows VPS.**

---

## 2. Real endpoints (verified live)

All Cloudflare-fronted and answer on **HTTPS 443 ONLY** — plain HTTP on `:90`/`:78` **times out**
(Cloudflare doesn't proxy those ports).

| Host | Service |
|---|---|
| `https://rigel-ms.wwiggles.org` | Mothership — login/auth + quest fetch |
| `https://rigel.wwiggles.org` | StationDb — `/register_server`, stations, **DashboardApiUrl target** |
| `https://rigel-eos.wwiggles.org` | EOS gateway — server list |

> **`https://rigel.wwiggles.org` is exactly 26 characters.** The client's dashboard URL is patched
> **in place** inside `libUnreal.so` over `https://api.oriondrift.net`, so **26 chars is a hard cap**.
> Nothing longer will fit.

**Never** run `cloudflared tunnel route dns --overwrite-dns`. Only ADD `rigel-*` records —
`heh.wwiggles.org` is a separate production host.

---

## 3. Where the bundle is

`rigel-server/` (copy it to the VPS):

```
.env.example      every deployment value in ONE place -> copy to .env
install.ps1       WINDOWS installer (backend + game server on one box)  <- USE THIS
install.sh        Linux installer (backend only) - not used here
backend/          .NET 6 source
windows/
  game/           the full A2 Nov15 build (3.1 GB) - bundled, nothing to fetch
  HalcyonA2.dll   injected payload
  server.config.psd1  game server config
  Start-Server.ps1 / Inject.ps1 / Build.ps1
docs/CONTABO-PORTS.md   opening ports (TWO firewalls!)
docs/DASHBOARD.md       control dashboard (READ THE SECURITY NOTE)
docs/GAME-SERVERS.md    game server config + why quests broke
```

### Install

From an **elevated** PowerShell:

```powershell
Copy-Item .env.example .env
notepad .env      # set PUBLIC_HOST, SERVER_API_KEY, META_APP_SECRET
.\install.ps1
```

`install.ps1` does: .NET install → applies `.env` into the compiled-in C# constants → publish →
`urlacl` for the privileged ports (50/78/80/90 need this on Windows) → firewall → optional
cloudflared → backend as a scheduled task (boot + survives logoff) → **auto-pairs the game server
to this box** (backend over `127.0.0.1`, `RegisterIp` = public IP).

Then: `cd windows ; .\Start-Server.ps1`

Flags: `-Status`, `-NoBuild`, `-NoFirewall`, `-Uninstall`.

---

## 4. IMMEDIATE next step — move the cloudflared tunnel

**This is the blocker.** The `rigel-*` DNS already points at tunnel **`rigel`**, but that tunnel is
still running on the **home PC**. Until it's moved, the domains resolve to the house, not the VPS.

1. On the home PC: `cloudflared tunnel list` → note the tunnel ID
2. Copy `%USERPROFILE%\.cloudflared\<TUNNEL-ID>.json` to the VPS
3. In `.env`:
   ```
   CLOUDFLARED_TUNNEL=rigel
   CLOUDFLARED_CREDENTIALS=C:\path\to\<TUNNEL-ID>.json
   ```
4. Re-run `.\install.ps1` — it installs cloudflared as a Windows service pointing at
   `127.0.0.1:90/78/50`
5. **STOP the tunnel on the home PC.** cloudflared load-balances between replicas, so leaving both
   running makes ~half of all requests hit the old backend and fail intermittently.

DNS needs **no** changes — you're only moving where the tunnel runs.

---

## 5. Current state

**Working / done:**
- Backend live and reachable on all three domains
- App Lab build **35232630** uploaded to **ALPHA** (Build ID `1359538833903947`) — this is the fix for the broken 35232629
- Netcode fixed: MI/MIB roof and the "ball flies across the map" bug (see §7)
- Removed a **third-party backend** that was hardcoded in the DLL (see §6)
- Stale stations cleaned; `OCE_1` ghost seed removed

**Outstanding:**
- Move the cloudflared tunnel (§4)
- **Rotate `SERVER_API_KEY`** — the old value shipped as a string literal inside a distributed DLL. Change it in `.env` **and** `MothershipServer.cs`, pass via `-ServerApiKey=`
- **Close `9095`/`9100`** (orchestrator) — they were open to the internet; anything that reaches them can spin up servers on the fleet
- **Dashboard auth is OFF** (`DashboardAuthPreprocessor.Enforce = false`, secrets are literal `CHANGE-ME-…`). Keep `:8080` firewalled; reach it over an SSH/RDP tunnel. See `docs/DASHBOARD.md`
- Remove `-NoAuthGate` from the game server's `ExtraArgs` before any public run
- Verify quests end-to-end after installing build 35232630

---

## 6. Hardcoded values that were fixed (do not regress)

The DLL shipped with **`157.173.194.216`** hardcoded for BOTH `/register_server` and the quest
fetch — **that is not this project's server**, so every station was registering with, and pulling
player quest data from, a third party. There was also a stale AWS fallback IP `34.239.141.19`.

Root cause it was an IP at all: the DLL's WinHTTP calls passed flags `0` = **plain HTTP only**, so it
physically could not reach the Cloudflare-fronted domains. Both call sites now set
`WINHTTP_FLAG_SECURE` when `port == 443`.

Now configurable (no rebuild needed) — set via `ExtraArgs` in `server.config.psd1`:

```
-MothershipHost=rigel-ms.wwiggles.org  -MothershipPort=443
-BackendHost=rigel.wwiggles.org        -BackendPort=443
-ServerName=Rigel                      -ServerApiKey=<64-hex>
```

Public-IP detection now **fails loudly and skips registration** rather than publishing a wrong
address. Use `RegisterIp` if detection fails.

---

## 7. Netcode fixes (already in the DLL — don't undo)

- **`[TEMP-YANK]`** (default ON, `-NoTempYank` to A/B) — the resync used to write the **shared** sim
  frame permanently, so two players on different epochs ping-ponged it every ingest
  (`resync=90-107/s`) and the MI loop counted the other player's whole buffer as missed. Now it's a
  call-scoped swap. Result: `resync` → **0**, MI peak **22-31 → 2-3**.
- **`[YANK-BUDGET]`** (default 3/player/sec, `-YankBudget=N`) — a stalled client dumping a batch of
  stale frames produced `resync=49` in one second; force-feeding those into the sim caused a huge
  rollback = **"the ball goes flying"**. Over budget, the native gate drops them instead.
- **`[ORPHAN-RELEASE]`** — the pump used to stick to an emptied sim whose frame ran away, so nobody
  could re-seat.

Note: `[SEND] peak` still reads 43-44 *during* a client stall. That is now a **truthful** signal
(the client really stopped sending) — do not "fix" that number.

Failed approaches, **do not retry**: `[FRAME-REBASE]` (rewriting `inputData+4` feeds back — the
buffer is re-read — and bricked rejoins) and `[HOLD-AT-TARGET]` (froze the sim: `frameAdvViaStep`
90 → 1-12). Both are default-off.

---

## 8. Quests — why they were broken

`[QUEST] uninit comp PlayerQuestComponent ... org ''` repeating forever is the tell.

Chain: client calls `Server_LoginToStationDashboard` → server stores an **org id** on the
PlayerController (`+0xA30`) → the DLL reads that org to init `PlayerQuestComponent`. No org → no
quests, ever.

Two causes, both fixed:
1. `DashboardApiUrl`/`DashboardApiKey` were blank → no dashboard login. Must be
   `https://rigel.wwiggles.org` + `halcyon-server-key` (= `A2StationDbServer.ServerMasterKey`).
2. The client pointed at the **LAN IP** while the server queried the remote backend, so the org
   never matched. The **mothership URL lives inside the OBB**, so fixing it required a full rebuild
   + App Lab upload — that is build 35232630.

Verify after installing 35232630: the `org ''` spam should stop and become
`[QUEST] login org=… / fetched N real quests / register … IsInitialized ->1`.

---

## 9. Client rebuild — the traps that cost the most time

**Never shortcut a versionCode bump.** It ripples into THREE places:

1. `apktool.yml` versionCode
2. the **OBBData version string in `classes.dex`** (same length)
3. the **OBB size literal in `classes.dex`** must equal the real OBB bytes

Build 35232629 was DOA because only #1 was done: the dex still named the old OBB, so UE's
**DownloaderActivity** fired and the app exited. Symptom: `signal 9 (Killed)`, **zero UE output**,
flashes into immersive then back to vrshell. That's an OBB mismatch, **not** entitlement.

**apktool gotcha that hides it:** the source `AndroidManifest.xml` has **no** `versionCode`
attribute — apktool injects it from `apktool.yml` at build time, and it will print
*"AndroidManifest.xml and resources have not changed"* and reuse a cached manifest.
**`rm -rf work/decode/build` first**; the log must then say *"Building resources with aapt2"*.

**Full pipeline:** edit `apktool.yml` + patch dex version string (repair dex SHA-1 + adler32) →
`apktool b` (cache cleared) → `build_client.py` (URLs + size literal + dex hashes) →
`inject_hook.py` (the EOS hook — without it the client hangs on the splash) → rename OBB to
`main.<code>.org.zern.rigel.obb` → `--validate` → upload.

**Always sideload before uploading.** A sideload always ends in `System.exit` (entitlement — only an
App Lab channel install is store-recognized), so "it exited" proves nothing. Read these instead:
- **no `DownloaderActivity`** ⇒ OBB/version/size all consistent
- `RigelHook: [ENTITLEMENT] bypass ARMED` ⇒ hook loaded
- `RigelHook: [EOSREDIR] https://api.epicgames.dev/... -> https://rigel-eos.wwiggles.org/...` ⇒ EOS redirect works
- `AppManagerInternal: Entitlement for packageName=org.zern.rigel not found` ⇒ only entitlement left, App Lab fixes it

Upload: `%APPDATA%\odh\ovr-platform-util.exe upload-quest-build --app-id 1358918123966018
--app-secret <secret> --apk <apk> --obb <obb> --channel ALPHA` (`--validate` first).

---

## 10. Ports — TWO firewalls

Contabo has an **external firewall in the customer panel** *and* the VPS has its own. Both must
allow the traffic. See `docs/CONTABO-PORTS.md`.

**The one everyone misses: `7777-7787/UDP`** — the game traffic is UDP. If only TCP is open,
players log in fine and then can't join anything.

With cloudflared you do **not** need to open 90/78/50 at all — the tunnel reaches them over
loopback. You only need **UDP 7777-7787** inbound (and RDP/SSH for yourself).

---

## 11. Useful commands

```powershell
.\install.ps1 -Status                     # backend + game servers + listening ports
cd windows ; .\Start-Server.ps1            # start a station
Get-Content <publish>\logs\*.txt -Tail 40  # backend log

# stations (source of truth for the in-game browser)
curl.exe -s -H "x-api-key: halcyon-server-key" https://rigel.wwiggles.org/stations
# delete a ghost station
curl.exe -s -X DELETE -H "x-api-key: halcyon-server-key" https://rigel.wwiggles.org/stations/<id>
```

Game-server log: `%TEMP%\HalcyonA2.log` (+ `HalcyonA2-console.log`). Useful tags:
`[QUEST] [SEND] [IN] [MI] [SEATDRIVE] [STEP] [REBASE] [NETRATE] [POSERATE]`.

**Healthy 2-player arena looks like:** `seated=2`, `[IN] resync=0`, `[SEND] missedInputs=0-2`
`missedCaught=44`, `[MI] spread=0-5`, `[STEP] frameAdvViaStep≈90 hitches>55ms/s=0`.

---

## 12. Facts worth keeping

- `BuildVersion`/`BuildIdNum` must equal **22284** or every station is filtered out of the browser
  ("login works, no stations" is almost always this).
- `matchmaking.json` must stay `"sessions": []` — it used to seed a fake `OCE_1` station at
  `127.0.0.1:7777` into every player's browser.
- `UA2PlayerEntity` on 22284: `FrequentDataReplicationOnly@0xF0` is the **only** pose struct and is
  already the replicated one; `0x308` is a `TArray`, **not** a second copy. The object ends ~`0x338`.
  The port had 20996 offsets and was writing past the end of the object at ~85 Hz/player — fixed.
  `AVRPawn.Entity` is `@0x928` (not `0x840`).
- App: ID `1358918123966018`, package `org.zern.rigel`, signing cert SHA-256
  `bee5fc0706704f1ed40226b7e0d95481a34f679562fcde253d2ff17bd51c1c9c` (pinned as
  `QUEST_EXPECTED_CERT_SHA`; Meta does **not** re-sign).
- Meta attestation is real (`graph.oculus.com/platform_integrity/verify`). **Never** set
  `MOTHERSHIP_INSECURE=1` on a public server.
