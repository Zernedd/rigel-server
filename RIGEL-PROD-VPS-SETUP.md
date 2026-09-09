# Rigel — Production Deployment on a VPS

End-to-end guide to run the Rigel stack (Orion Drift / A2, build **22284**) on a public VPS behind
your own domain, publish the client to Meta App Lab, and spin up game servers (stations) that show
up in the in‑headset browser.

This is the "get off LAN and onto a domain" playbook. It assumes you have already, at least once,
built and validated the client on LAN (see `Rigel\` in this repo). Nothing here overwrites any
existing DNS — it only **adds** the `rigel-*` subdomains.

---

## 0. Architecture — what runs where

```
  QUEST (Rigel client, org.zern.rigel, App Lab install)
     │  HTTPS (443)                              UDP 7777 (game)
     ▼                                              │
  ┌─────────────────────────── VPS (public IP) ────┼────────────────────┐
  │  Caddy (TLS 443, auto Let's Encrypt)            │                    │
  │    rigel-ms.<domain>   ──► 127.0.0.1:90   Mothership (login/auth)    │
  │    rigel.<domain>      ──► 127.0.0.1:78   StationDb (roles/stations) │
  │    rigel-eos.<domain>  ──► 127.0.0.1:50   EOS gateway (server list)  │
  │                        └─► 127.0.0.1:80   EOS websocket (lobby/RTC)  │
  │                                                 │                    │
  │  OrionDriftBackend (Ares, .NET 6, single proc)  │                    │
  │  A2 dedicated game server(s) (Nov15 + HalcyonA2.dll) ◄───────────────┘
  │    self-register via /register_server, listen UDP 7777(+)            │
  │  HalcyonSocket orchestrator :9095 (control) / :9100 (agent TCP)      │
  └──────────────────────────────────────────────────────────────────────┘
```

Key facts that drive the whole design:

- **HTTP services can live behind a domain + TLS.** Login, station data and the EOS server list are
  all HTTPS and go through Caddy on 443.
- **The game socket is UDP and cannot be proxied by a normal reverse proxy or a Cloudflare tunnel.**
  It must be a **directly reachable IP:port** on the VPS (open UDP 7777+ in the firewall). This is
  why a VPS with a public IP — not a home box behind a Cloudflare tunnel — is the real production
  answer: the game servers run on the same box the domain points at.
- **The client reaches EOS only because of the runtime hook.** `libRigelHook.so` rewrites the EOS
  SDK's `api.epicgames.dev` calls to your `rigel-eos.<domain>` gateway. Without it the client talks
  to real Epic and hangs on the loading screen.
- **Entitlement is real, not bypassed.** The client is an App Lab build your test account owns, so
  Meta attestation passes legitimately. `MOTHERSHIP_INSECURE` is **never** used in production.

Pick your subdomains once (examples used throughout — 26‑char cap on the dashboard one, see §5):

| Purpose | Subdomain | Proxies to |
|---|---|---|
| Mothership (login) | `rigel-ms.<domain>` | `127.0.0.1:90` |
| StationDb (dashboard API) | `rigel.<domain>` (**≤ 26 chars incl. `https://`**) | `127.0.0.1:78` |
| EOS gateway + WS | `rigel-eos.<domain>` | `127.0.0.1:50` (and `:80` for WS) |

> `https://rigel.wwiggles.org` is 26 chars — exactly the limit for the in‑place libUnreal patch.

---

## 1. VPS prerequisites

- A Linux VPS (Ubuntu 22.04+ assumed) with a **public IPv4** and root/sudo.
- **.NET 6 runtime**: `sudo apt-get install -y dotnet-runtime-6.0` (or the SDK if you build on the box).
- **Caddy** (auto‑TLS reverse proxy): https://caddyserver.com/docs/install
- Firewall open: **TCP 443** (Caddy) and **UDP 7777–7787** (game servers). Everything else stays local.
  ```bash
  sudo ufw allow 443/tcp
  sudo ufw allow 7777:7787/udp
  ```
- The backend binds low ports (50/78/80/90) locally. On Linux that needs the bind capability — grant
  it to the dotnet host once (or run the unit as root):
  ```bash
  sudo setcap 'cap_net_bind_service=+ep' "$(readlink -f "$(which dotnet)")"
  ```

---

## 2. DNS — add the rigel subdomains (do NOT touch existing records)

In your DNS provider, **add** three records pointing at the VPS public IP. Leave every existing
record (e.g. `heh.<domain>`) exactly as it is.

```
A   rigel-ms.<domain>    <VPS_PUBLIC_IP>
A   rigel.<domain>       <VPS_PUBLIC_IP>
A   rigel-eos.<domain>   <VPS_PUBLIC_IP>
```

If the zone is on Cloudflare: set these three to **DNS only (grey cloud)**, not proxied. The EOS SDK
and UE HTTP layer want a direct TLS endpoint; Caddy on the VPS terminates TLS with a real Let's
Encrypt certificate, which the EOS SDK's CA bundle trusts. (Cloudflare's proxy would also work for the
HTTP services, but grey‑cloud keeps the game/EOS path simplest and avoids the 100s/websocket edge
quirks.)

---

## 3. Reverse proxy + TLS (Caddy)

`/etc/caddy/Caddyfile`:

```caddyfile
rigel-ms.<domain> {
    reverse_proxy 127.0.0.1:90
}

rigel.<domain> {
    reverse_proxy 127.0.0.1:78
}

rigel-eos.<domain> {
    # EOS REST on :50; the SDK also opens a websocket which Caddy upgrades transparently.
    reverse_proxy 127.0.0.1:50
    # If lobby/RTC websockets need the :80 server, split by path prefix instead:
    # @ws path /stomp* /rtc* /lobby*ws*
    # reverse_proxy @ws 127.0.0.1:80
}
```

```bash
sudo systemctl reload caddy
```

Caddy fetches certs automatically on first request. Verify:
`curl -I https://rigel-ms.<domain>/` → HTTP 200.

---

## 4. Run the backend (production, real attestation)

Copy `OrionDriftBackend/` to the VPS (or build there). Publish once:

```bash
cd OrionDriftBackend
dotnet publish src/AUnrealFeatures.Ares -c Release -o /opt/rigel/backend
```

The backend already verifies Quest attestation against `graph.oculus.com` using your app's
server‑to‑server credentials, set in `MothershipServer.cs`:

```csharp
const string META_APP_ID     = "1358918123966018";
const string META_APP_SECRET = "fd2e8b4be45e99a72ce8486a0ac3b8f1";   // rotate if leaked
QUEST_EXPECTED_PACKAGE  = "org.zern.rigel";
QUEST_EXPECTED_CERT_SHA = "bee5fc0706704f1ed40226b7e0d95481a34f679562fcde253d2ff17bd51c1c9c";
```

Point the EOS endpoint table at your domain (these are served at `/sdk/v1/default`):
`src/AUnrealFeatures.EOSSDK/.../Responses/custom_endpoints.json` and `default_endpoints.json` —
replace the host (currently `192.168.1.29:50`) with `https://rigel-eos.<domain>` (drop the port —
Caddy is on 443). Rebuild after editing (they are embedded resources).

systemd unit `/etc/systemd/system/rigel-backend.service`:

```ini
[Unit]
Description=Rigel backend (Ares)
After=network.target

[Service]
WorkingDirectory=/opt/rigel/backend
ExecStart=/usr/bin/dotnet /opt/rigel/backend/AUnrealFeatures.Ares.dll
Restart=always
User=rigel
# PRODUCTION: do NOT set MOTHERSHIP_INSECURE. Do NOT set MOTHERSHIP_ALLOW_NONSTORE.
# Environment=MOTHERSHIP_ALLOW_NONSTORE=1   # ONLY for sideloaded-hook dev iteration

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl enable --now rigel-backend
```

> **Never** set `MOTHERSHIP_INSECURE=1` on a public server — it disables identity verification
> entirely (anyone can claim any account). `MOTHERSHIP_ALLOW_NONSTORE=1` is a dev‑only switch to test
> a *sideloaded* hook build without a 5‑minute App Lab re‑upload; it relaxes only the
> "installed from store" check (package/cert/user‑proof are still enforced). Both must be OFF in prod.

Windows note: on a Windows host the low ports instead need one‑time URL ACLs —
`Rigel\tools\Grant-Ports.ps1` (elevated) — and inbound firewall rules — `Rigel\tools\Grant-Firewall.ps1`.
The self‑elevation in `ModuleInitialization` is disabled (gated behind `MOTHERSHIP_SELF_ELEVATE=1`).

---

## 5. Build & publish the Rigel client for the domain

On your build box (`Rigel\`), point everything at the domain, then upload to App Lab.

1. **`Rigel\config.json`** — set the domain hosts:
   ```json
   {
     "mothership": "https://rigel-ms.<domain>",
     "dashboard":  "https://rigel.<domain>",      // MUST be ≤ 26 chars incl. https://
     "verify_obb": false
   }
   ```
2. **Build the APK + OBB** (`Rigel\Build.bat`). This rewrites the OBB's Mothership `BaseUrl`, patches
   the dashboard URL in `libUnreal.so` (≤ 26 chars), and retargets the OBB size literal.
3. **Rename to our own package** so Meta doesn't treat it as a modified store app (kills the
   "unofficial app" dialog): `python rename_package.py out\...\AndroidManifest.xml --package org.zern.rigel`
   (already wired into the Rigel build; the package is `org.zern.rigel`).
4. **Patch the OBB's Meta App ID** to yours so entitlement keys off your app:
   ```
   python ..\HalcyonA2\tools\a2urlpatch.py patch <obb> --old <oldMobileAppId> --new 1358918123966018 --write
   ```
5. **Point the EOS hook at the domain.** In `Rigel\hook\rigel_hook.cpp` set:
   ```cpp
   static const char* kEosGateway = "https://rigel-eos.<domain>";   // was http://192.168.1.29:50 (LAN)
   ```
   Rebuild the hook (`hook\build_hook.ps1`) and inject it (`hook\inject_hook.py`). The hook rewrites
   the EOS SDK's `api.epicgames.dev` bootstrap to your gateway; with a real TLS cert (Caddy) it stays
   `https://`, which the EOS SDK's CA bundle trusts.
6. **Upload to App Lab** (`%APPDATA%\odh\ovr-platform-util.exe`):
   ```
   ovr-platform-util upload-quest-build --app-id 1358918123966018 --app-secret <SECRET> \
       --apk <hooked.apk> --obb <patched.obb> --channel ALPHA --age-group TEENS_AND_ADULTS
   ```
   Validate first with `--validate`. First upload needs the org to have signed the Developer
   Distribution Agreement.
7. **Add testers**: Dashboard → your app → Distribution → Release Channels → ALPHA → add the Oculus
   username. Those accounts install from the channel (store‑recognized → entitlement passes).

---

## 6. Adding stations (game servers)

A "station" in the browser is a **running dedicated game server** that has registered itself. The
EOS server list (`/matchmaking/v1/{deployment}/filter`) is the backend's in‑memory session list; a
server appears there when it calls `POST /register_server`, and is stamped with the fleet build
(`BuildVersion` — **must be `22284`** or the 22284 client filters it out; already set).

### 6a. Manually (one server)

Run the A2 dedicated server (Nov15 build + `HalcyonA2.dll` injected) on the VPS. On spin‑up it
`POST /register_server` to the StationDb with its IP/port, which creates a station + deployment and
an EOS session pointing at `IP:UDP_PORT`. Give each server a distinct UDP port (7777, 7778, …) and a
`server_name`. Ensure that UDP port is open in the firewall and the advertised IP is the VPS public
IP (not 127.0.0.1).

### 6b. Orchestrator ("Spin Up" button)

The dashboard's **Spin Up** calls the in‑process `HalcyonSocketServer` (control `:9095`, agent TCP
`:9100`). A **HalcyonAllocatorAgent** running on a host box connects to `:9100`; Spin Up mints a
deployment id + name and relays a launch request to the least‑loaded connected agent. The agent
launches and injects an A2 server with `-DashboardDeploymentId=<id>`; the server then
self‑registers under that id (upsert, so re‑launches reuse the same station). The name typed in the
Spin Up modal sticks via `PendingNames`.

To use it: run the backend with the EOS gateway + Halcyon socket enabled (both are on by default in
`Program.cs`), start an allocator agent on each host that will run servers, and use the dashboard
Spin Up modal (name + region + max players). Servers appear in the browser within a poll cycle.

### 6c. The placeholder seed

`Responses/matchmaking.json` ships one placeholder session (now build 22284) so the browser is not
empty before any real server registers. It points at `127.0.0.1:7777` and is **not joinable** — it
only proves the list pipe. Delete it (and any `sessions.json`) once you have real servers, or leave
it as a "coming soon" tile.

---

## 7. Verifying the whole chain

Watch the backend log while a test account launches Rigel from the ALPHA channel:

1. `POST /users/log_in`, `/v2/player/client/auth/begin|complete/QUEST` → `[QUEST-ATT] attOk=True
   app=StoreRecognized proofOk=True` — login + real attestation.
2. EOS gateway hits: `/sdk/v1/default` → `/auth/v1/oauth/token` → `/sdk/v1/product/...` →
   `/matchmaking/v1/.../filter` — the redirect + server list.
3. On the client: past the splash → station browser populated with your `22284` servers.
4. Join a station → client connects over UDP to the advertised `IP:7777`.

---

## 8. Troubleshooting (lessons already paid for)

| Symptom | Cause | Fix |
|---|---|---|
| "Unofficial app" dialog every launch | re‑signed build kept `com.AnotherAxiom.A2` | own package `org.zern.rigel` (done) |
| Game quits ~4 s after TOS (`System.exit(0)`) | not entitled | App Lab build the account owns; OBB `MobileAppId` = your App ID |
| Stuck on **loading screen**, OBB is valid | client can't reach the mothership/EOS | open firewall; EOS hook must point at a reachable gateway |
| Loading screen, login works, `:50` gets 0 hits | EOS SDK talking to real Epic | the `libRigelHook` curl redirect is missing/points at the wrong host |
| Browser **empty**, list is served | server build ≠ client build | `BuildVersion`/`BuildIdNum` = `22284` (done) |
| Login denied, `[QUEST-ATT] tamper=app_integrity=...` | sideloaded (not store) | install from the ALPHA channel, or dev‑only `MOTHERSHIP_ALLOW_NONSTORE=1` |
| Can see a station but can't join | UDP not reachable | open UDP 7777+, advertise the public IP not 127.0.0.1 |

---

## 9. Security checklist before going live

- [ ] `MOTHERSHIP_INSECURE` and `MOTHERSHIP_ALLOW_NONSTORE` **unset** in the systemd unit.
- [ ] App secret rotated if it was ever pasted into a log/chat, and not committed to the repo.
- [ ] Only 443/tcp and the game UDP range are open to the internet; 50/78/80/90/9095/9100 stay local.
- [ ] Dashboard admin routes gated (SSO) before exposing `rigel.<domain>` publicly.
- [ ] Existing DNS records (e.g. `heh.<domain>`) untouched.
```

Generated with [Claude Code](https://claude.com/claude-code)
