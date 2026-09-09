# The global dashboard

There are **two** front-ends over **one** control API. The API is
`http://<backend>:8080/api/*`, served by `AresDashboardServer`.

| | Port | What it is |
|---|---|---|
| **Full UI** (recommended) | **3000** | The React/Next.js app in `backend/dashboard/`. Overview, Users, Stations, Station detail, Deployments, Events, Telemetry, WS Monitor. |
| Basic UI + the API itself | 8080 | A single static `index.html` the backend serves directly. No Node needed. |

The full UI holds **no state and no secrets** — `next.config.ts` just proxies
`/api` + `/auth` to `:8080` and `/game` to `:78` (writes go to StationDb to avoid
LiteDB concurrent-access errors).

`install.ps1` installs, builds, and registers the full UI automatically as the
`RigelDashboardUi` startup task, **if Node.js 20+ is on PATH**. If Node is missing it
prints a warning, skips it, and everything else still works. To add it later:

```powershell
winget install OpenJS.NodeJS.LTS   # then open a NEW shell so PATH updates
.\install.ps1 -NoBuild             # skips dotnet publish, still does the UI
```

Set `INSTALL_DASHBOARD_UI=0` in `.env` to opt out, or pass `-NoDashboardUi`.

It binds **127.0.0.1 only** (`next start -H 127.0.0.1`), so it is not reachable from
the internet even if port 3000 were open.

---

## Authentication (ENFORCED as of 2026-09-09)

`/api/*` on :8080 requires **either** a valid session cookie **or** the admin key header.
Auth used to be `Enforce = false` with `CHANGE-ME` placeholders, which meant anyone who
could reach the port controlled the whole fleet. That is fixed:

- `DashboardAuthPreprocessor.Enforce` now defaults to **on** (`DASHBOARD_AUTH_ENFORCE=0`
  disables it, for local debugging only).
- Secrets come from the **environment**, never from source: `DASHBOARD_ADMIN_KEY` and
  `DASHBOARD_SESSION_SECRET`. If either is unset the server generates a random one at
  startup and prints it once — it can no longer silently fall back to a placeholder.
- On the VPS these are set as **machine-level** environment variables, because `install.ps1`
  only reads `.env` at install time and does not export it to the running service.

The dashboard page prompts for the key on first load, keeps it in `localStorage` for that
browser only, and sends it as `x-dashboard-key` on every `/api` call (fetch is wrapped once
rather than at each call site). The key never appears in a URL, so it cannot leak via logs,
history, or a `Referer`. A 401 re-opens the prompt; **Lock** forgets the key.

Scripts authenticate the same way:

```bash
curl -H "x-dashboard-key: $DASHBOARD_ADMIN_KEY" https://rigel-dash.wwiggles.org/api/stations
```

## Public endpoint

Reached through the **existing cloudflared tunnel** (`948b07c7-…`), which already fronts
`rigel-ms` / `rigel` / `rigel-eos`. A fourth hostname was **added** to its ingress — the
existing rules were not touched:

```yaml
  - hostname: rigel-dash.wwiggles.org
    service: http://127.0.0.1:8080
```

It needs a DNS CNAME `rigel-dash` → `948b07c7-b3e6-4985-9620-6d8d10711b67.cfargotunnel.com`
(proxied). `cloudflared tunnel route dns` could not create it — the origin `cert.pem` on the
box is a 282-byte stub, not a real cert — so add the record in the Cloudflare dashboard.

**Port 8080 is firewalled from the internet** (`Rigel-Block-Dashboard-8080-Inbound`). The
tunnel reaches it over loopback, so it is unaffected; the only paths in are the tunnel and an
SSH tunnel.

## Accessing it safely today (SSH tunnel — recommended)

Keep 3000 and 8080 firewalled off and forward them over SSH:

```bash
ssh -L 3000:127.0.0.1:3000 -L 8080:127.0.0.1:8080 Administrator@<your-vps>
```

Then browse **<http://localhost:3000>** on your own machine (the full UI). The traffic is
encrypted by SSH, nothing is exposed publicly, and it works even with `Enforce=false`.

If you are already on the VPS over RDP, just open `http://localhost:3000` in a browser
there — no tunnel needed.

Verify the API directly:

```bash
curl -s http://localhost:8080/api/stations | jq
```

---

## Hardening it (before any public exposure)

1. **Set real secrets** in `DashboardAuthPreprocessor.cs`:
   ```csharp
   const string SessionSecret     = "<openssl rand -hex 32>";
   const string DashboardAdminKey = "<openssl rand -hex 32>";
   ```
2. **Turn auth on** — `Enforce = true` (line 23), and `AUTH_ENFORCE = true` in
   `backend/dashboard/middleware.ts:7`.
3. **Set the real origin** — `DashboardServer.cs:78`
   `DASHBOARD_ORIGIN = "http://localhost:3000"` must become your real dashboard URL,
   and that URL must be registered as a Meta SSO `redirect_uri`.
4. Rebuild + redeploy (`sudo ./install.sh`).
5. Confirm the log shows `[DASH-AUTH] 401 …` for unauthenticated calls (not
   `would-401`), then expose it.

Scripts/automation authenticate with the admin key header instead of an SSO session:

```bash
curl -H "x-api-key: <DashboardAdminKey>" https://rigel-dash.<domain>/api/stations
```

---

## Exposing it on a subdomain (after hardening)

Add a DNS A record `rigel-dash.<domain>` → your VPS, then in `/etc/caddy/Caddyfile`:

```caddy
rigel-dash.<domain> {
    reverse_proxy 127.0.0.1:8080
}
```

`systemctl reload caddy`. Caddy terminates TLS, so 8080 itself still stays closed in
`ufw` — only 443 is public.

---

## What you can control from it

- **Stations** — list, inspect, delete; per-station config (the info-board texture URLs
  and `StationAnnouncement` that the game reads as NetVars)
- **Deployments** — the running server instances behind each station
- **Users** — roles, kick, mute, warn, ban
- **Orchestrator** — *Spin Up* launches + injects a new game server on the least-loaded
  connected agent (agents connect to `:9100`)
- **Server events** — the per-deployment event feed

---

## Useful API calls

```bash
D=http://localhost:8080          # via the SSH tunnel
S=https://rigel.wwiggles.org     # StationDb, key: halcyon-server-key

# stations (StationDb is the source of truth the browser uses)
curl -s -H "x-api-key: halcyon-server-key" $S/stations

# remove a stale/ghost station
curl -s -X DELETE -H "x-api-key: halcyon-server-key" $S/stations/<station_id>

# dashboard API
curl -s $D/api/stations
```

> **Ghost stations:** a station row persists after its game server dies. If the browser
> shows something unjoinable, delete it with the `DELETE /stations/{id}` call above.
> The EOS session seed (`Responses/matchmaking.json`) must stay `"sessions": []` — it
> previously seeded a fake `OCE_1` server at `127.0.0.1:7777` into every player's browser.
