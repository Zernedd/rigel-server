# Game servers (Windows) + enabling quests

The **backend** (this bundle) runs on the Linux VPS. The **game servers** are
`A2-Win64-Shipping.exe` with `HalcyonA2.dll` injected, and must run on **Windows** —
they can't run on the VPS. They connect out to the backend.

---

## Pointing a game server at the backend

On the Windows box, edit `HalcyonA2\tools\server.config.psd1`:

This is the **live, verified-working** configuration (`server.config.psd1`):

```powershell
    # REQUIRED FOR IN-GAME QUESTS (and the station info-boards).
    # StationDb, behind Cloudflare on 443. Exactly 26 chars — see the client note below.
    DashboardApiUrl = 'https://rigel.wwiggles.org'
    DashboardApiKey = 'halcyon-server-key'
    DeploymentId    = ''      # blank = backend mints one

    ExtraArgs = @(
        '-MothershipHost=rigel-ms.wwiggles.org'   # login/auth + quest fetch
        '-MothershipPort=443'
        '-BackendHost=rigel.wwiggles.org'         # register_server + player-count heartbeat
        '-BackendPort=443'
    )
```

Then `.\Start-Server.ps1`. Confirm it reached **your** backend:

```
[HalcyonA2] -MothershipHost=rigel-ms.wwiggles.org
[HalcyonA2] -BackendHost=rigel.wwiggles.org
[HalcyonA2] register IP = <this box's public ip>
[HalcyonA2] register_server -> {"success":true,"station_id":"…","deployment_id":"…"}
```

> ### ⚠ These pointed at someone else's server
> The DLL shipped with `kBackendHost = "157.173.194.216"` (register + player-count) and
> `g_motherHost = "157.173.194.216"` (login + **player quest data**). That IP is **not
> this project's**, so every station registered with, and pulled quests from, a third
> party's backend. There was also a public-IP fallback of `34.239.141.19` (stale AWS)
> that would publish a wrong connect address to every client.
>
> **Why it was an IP and not the domain:** the DLL's WinHTTP calls passed flags `0` —
> plain HTTP only — so it physically could not reach the Cloudflare-fronted domains
> (`rigel-*.wwiggles.org` answer on **443 only**; `:90`/`:78` time out). Both call sites
> now set `WINHTTP_FLAG_SECURE` when `port == 443`.
>
> The IP-detect fallback now **fails loudly and skips registration** rather than
> publishing a stranger's address. Use `RegisterIp` if detection fails.
>
> **Rotate `SERVER_API_KEY`** — the old one (`7b93…`) was a string literal inside the
> distributed DLL. It must be changed in lockstep with `MothershipServer.cs SERVER_API_KEY`,
> then passed via `-ServerApiKey=`.

### The client must point at the SAME backend

`Rigel/config.json` was still on the LAN IP (`192.168.1.29`), so the headset logged into a
**local** backend while the DLL queried the **remote** one — the org never matched and
quests could never resolve. It is now:

```json
"mothership": "https://rigel-ms.wwiggles.org",
"dashboard":  "https://rigel.wwiggles.org",
```

**HARD LIMIT:** the dashboard URL is patched *in place* over `https://api.oriondrift.net`
inside `libUnreal.so`, so it must be **≤ 26 characters**. `https://rigel.wwiggles.org` is
**exactly 26** — it fits, but any longer host will not patch. The mothership URL lives in
the OBB ini and has no length limit.

Changing these requires an **APK rebuild + reinstall** (`Rigel\Build.bat`, then install to
the headset).

---

## Why quests weren't working

Symptom in `%TEMP%\HalcyonA2.log`, repeating forever:

```
[QUEST] uninit comp PlayerQuestComponent pawn BP_VRPawn_C_N pc <null> org ''
```

`org ''` is the tell. The chain is:

1. The client calls `Server_LoginToStationDashboard`.
2. That stores an **org id** on the PlayerController (`+0xA30`).
3. The DLL reads that org and initialises each `PlayerQuestComponent`.
4. No org → `if (org.empty()) continue;` → **no quests, ever**.

Step 1 can't happen unless the server was launched with `-DashboardApiUrl` /
`-DashboardApiKey` / `-DashboardDeploymentId`. With those blank (the old default),
there is no dashboard login, so `org` stays empty.

**Fix:** set `DashboardApiUrl` + `DashboardApiKey` as above.

- `DashboardApiUrl` must point at **StationDb, port 78** — it serves
  `GET /v1/deployments/{id}?include_station_config=true`.
- `DashboardApiKey` must equal `A2StationDbServer.ServerMasterKey`, which is the
  literal `halcyon-server-key`.

**Verify it worked** — with a player connected you should see the `org ''` spam stop
and instead get:

```
[QUEST] login org=<something> tokenLen=NNN
[QUEST] fetched N real quests for org <something>
[QUEST] register PlayerQuestComponent ... with N REAL quests IsInitialized ->1
```

The same dashboard config also drives the station **info-boards**
(`BoardTextureUrl*`, `SignPlaza*`), so those stay blank without it too.

---

## Adding stations

A station in the browser = a running game server that has `POST /register_server`'d.

- **Manual:** launch a game server with a distinct UDP port; it self-registers.
  Advertise the **public** IP and open that UDP port in *both* Contabo firewalls.
- **Orchestrator:** run a `HalcyonAllocatorAgent` that connects to the backend's
  `:9100`; the dashboard's *Spin Up* launches and injects a server on the least-loaded
  agent. Keep `9095`/`9100` **private** (see `CONTABO-PORTS.md`).

`BuildVersion`/`BuildIdNum` must equal the client build (**22284**) or every station is
filtered out of the browser — this is the usual cause of "login works, no stations".

---

## Useful launch flags

| Flag | Effect |
|------|--------|
| `-NoAuthGate` | auth gate logs only, never kicks. **Testing only.** |
| `-NoTempYank` | revert the MI thrash fix (permanent frame yank) — A/B only |
| `-YankBudget=N` | resync yanks/player/sec (default 3; `0` = unlimited/old) |
| `-NoFrameRebase` | epoch rebase off (default; opt-in is `-FrameRebase`, known-buggy) |
| `-NoHoldTarget` | input-delay hold off (default; `-HoldTarget` freezes the sim — don't) |
| `-MothershipHost=` / `-MothershipPort=` / `-ServerApiKey=` | backend location + secret |
| `-NetSpeed=N` | per-connection bandwidth cap (default 200000 B/s) |

`-NoAuthGate` is currently enabled in `server.config.psd1` `ExtraArgs`. **Remove it
before any public run.**
