# Rigel server bundle

Everything needed to run the Rigel (Orion Drift / A2 build **22284**) private server —
backend, game server, and the game build itself. **~3.2 GB** (the bundled A2 Nov15 build
is 3.1 GB of that; everything else is ~2 MB).

## Install — Windows VPS (backend + game server on ONE box)

This is the setup in use. Copy the whole folder to the Windows VPS, then from an
**elevated** PowerShell:

```powershell
Copy-Item .env.example .env
notepad .env            # set PUBLIC_HOST, SERVER_API_KEY, META_APP_SECRET
.\install.ps1
```

That installs .NET, applies `.env` into the backend, publishes it, reserves the
privileged ports (`urlacl` for 50/78/80/90 — required on Windows), opens the firewall,
registers the backend as a **scheduled task** that starts at boot and survives logoff,
then **pairs the game server config to this same box**: the game server talks to the
backend over `127.0.0.1` (no TLS, no Cloudflare round-trip) while `RegisterIp` stays your
**public** IP, which is what clients are told to connect to.

Then start a station:

```powershell
cd windows
.\Start-Server.ps1
```

Useful: `.\install.ps1 -Status`, `-NoBuild` (re-apply config only), `-Uninstall`.

### Linux VPS instead?

`install.sh` does the same for the **backend only** — the game server is a Windows binary
(`A2-Win64-Shipping.exe` + an injected DLL using WinHTTP/SEH) and cannot run natively on
Linux, so it would need a separate Windows machine. `windows/game/` (3.1 GB) is not needed
on a Linux VPS.

---

## What's here

```
.env.example        every deployment-specific value, in one place
install.ps1         WINDOWS one-shot install (backend + game server on one box)  <- use this
install.sh          Linux one-shot install (backend only)
backend/            the .NET 6 backend source (Mothership, StationDb, EOS, Dashboard, orchestrator)
  dashboard/        optional Next.js UI (the backend already serves a UI on :8080)
windows/            the GAME server side — must run on Windows, not the VPS
  game/             the full A2 Nov15 build (3.2 GB) — BUNDLED, nothing to fetch
  HalcyonA2.dll     the injected payload
  server.config.psd1  pre-filled, paired with .env, points at .\game\
  Start-Server.ps1 / Inject.ps1 / Build.ps1
docs/
  CONTABO-PORTS.md  opening ports (BOTH firewalls — the UDP one is the gotcha)
  GAME-SERVERS.md   pointing game servers at the backend + why quests were broken
  DASHBOARD.md      accessing the control dashboard (read the security note first)
```

---

## The two halves

The backend is Linux; the **game servers are Windows-only** (`A2-Win64-Shipping.exe` +
`HalcyonA2.dll`) and cannot run on the VPS. They connect *out* to the backend, so they
can live anywhere with a public UDP port.

```
   Quest client ──HTTPS/443──> backend (VPS)      login, stations, EOS, quests
        │
        └────────UDP 7777+────> game server (Windows box)
                                     │
                                     └──HTTPS/443──> backend   register_server, quests
```

---

## Pairing (this is the part that bites)

These must agree or things fail in confusing ways:

| Value | Backend (`.env`) | Game server (`windows/server.config.psd1`) | Client (`Rigel/config.json`) |
|---|---|---|---|
| Shared secret | `SERVER_API_KEY` | `-ServerApiKey=` | — |
| Mothership | serves `:90` | `-MothershipHost/Port` | `mothership` |
| StationDb | serves `:78` | `DashboardApiUrl` + `-BackendHost/Port` | `dashboard` |
| Build | `BUILD_VERSION=22284` | `GameBuild='22284'` | APK build |

Mismatch symptoms:
- **wrong `SERVER_API_KEY`** → servers can't register / quests won't fetch
- **wrong build** → login works, **no stations listed** (all filtered out)
- **client and server on different backends** → login works, **no quests** (the org never matches)
- **`DashboardApiUrl` blank** → `[QUEST] uninit comp … org ''` forever, no quests

---

## Security checklist before going public

- [ ] **Rotate `SERVER_API_KEY`** — the old one shipped as a string literal inside the
      distributed DLL. Change it in `.env` *and* `MothershipServer.cs`, pass via `-ServerApiKey=`.
- [ ] **Dashboard auth is OFF** (`DashboardAuthPreprocessor.Enforce = false`) and both its
      secrets are `CHANGE-ME-…`. Keep `:8080` firewalled; use the SSH tunnel. See `docs/DASHBOARD.md`.
- [ ] **Keep `9095`/`9100` (orchestrator) private** — anything that reaches them can spin up
      servers on your fleet.
- [ ] Remove `-NoAuthGate` from the game server's `ExtraArgs`.
- [ ] `MOTHERSHIP_INSECURE` / `MOTHERSHIP_ALLOW_NONSTORE` stay `0`.
- [ ] Set a real `META_APP_SECRET` in `.env`.

---

## Notes carried over from debugging

- The DLL used to have **someone else's server** (`157.173.194.216`) hardcoded for both
  `/register_server` and the quest fetch, plus a stale AWS IP (`34.239.141.19`) as the
  public-IP fallback. Both are gone; the hosts are now flags, and a failed IP detect
  **refuses to register** rather than publishing a wrong address.
- The DLL now speaks **TLS on port 443**, which is what lets it use the real
  Cloudflare-fronted domains (they answer on 443 only; `:90`/`:78` time out).
- `Responses/matchmaking.json` must stay `"sessions": []`. It used to seed a fake
  `OCE_1` station at `127.0.0.1:7777` into every player's browser.
- The client dashboard URL is patched **in place** inside `libUnreal.so`, so it is capped
  at **26 characters**. `https://rigel.wwiggles.org` is exactly 26 — nothing longer fits.
