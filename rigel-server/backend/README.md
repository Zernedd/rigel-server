# OrionDriftBackend

The Mothership + station/dashboard backend for the private server — the two services the
game and the game-server DLL actually talk to, with everything else stripped out.

Assembled from `Documents\AUnrealFeatures-main`. That repo also contains a PlayFab clone,
Virtex, a content-download network and the EOS gateway; none of it is needed to run a
Mothership and a dashboard, so none of it is here.

## What runs

| Port | Server | Who talks to it |
|---|---|---|
| **90** | `MothershipServer` | the **game client** — login and player data. This is the `BaseUrl` you patch into the client. |
| **78** | `A2StationDbServer` | the **game server DLL** — `POST /register_server`, `POST /update_player_count`, and the native station-config fetch `GET /v1/deployments/{id}?include_station_config=true`. This is the `-DashboardApiUrl` / `api.oriondrift.net` side. |
| **8080** | `AresDashboardServer` | the **dashboard UI** — read API, login, telemetry. |
| 3000 | Next.js dev server | the dashboard web UI itself. |

These are the ports the code actually binds (the original repo's README quotes 5000/7811,
which is out of date).

## Layout

```
OrionDriftBackend.sln
run-backend.cmd            starts the three servers
run-dashboard.cmd          starts the dashboard UI (installs packages on first run)
src\
  AUnrealFeatures.Ares\               host + A2StationDbServer + AresDashboardServer
  AUnrealFeatures.AAMothership\       MothershipServer
  AUnrealFeatures.Hosting\            HTTP framework + LiteDB layer
  AUnrealFeatures.DependencyInjection\  DI container used by Hosting
  AUnrealFeatures.EOSSDK\             kept only as a compile dependency (see below)
  AUnrealFeatures.HalcyonSocket\      kept only as a compile dependency (see below)
dashboard\                 the Next.js dashboard UI
```

## Running

```powershell
run-backend.cmd            # needs the .NET 6 SDK
run-dashboard.cmd          # needs Node.js; dashboard on http://localhost:3000
```

The .NET 6 SDK is at <https://dotnet.microsoft.com/download/dotnet/6.0>, Node at
<https://nodejs.org>. Neither was installed on this machine when the folder was built, so
**this has not been compiled or run yet** — see *Status* at the bottom.

Ports 78 and 90 are privileged-range; if binding fails, start the backend from an
elevated prompt.

Point the dashboard at a different backend with an environment variable:

```powershell
$env:BACKEND_HOST = "157.173.194.216"   # default is 127.0.0.1
run-dashboard.cmd
```

Debug builds use the password-less `development` LiteDB file; Release uses `production`.

## How the client and server connect to this

Both URLs are patched into the client with `..\HalcyonA2\tools\build_client.py`:

```powershell
python build_client.py --mothership http://<your-host>:90 --dashboard http://<your-host>:78
```

Remember the Dashboard URL is patched in place inside `libUnreal.so`, so it must be **26
characters or fewer** — an IP and port fits (`http://192.168.1.50:78` is 22).

The game-server DLL side already matches this backend out of the box:

| DLL constant (`HalcyonA2\HalcyonA2\dllmain.cpp`) | This backend |
|---|---|
| `kBackendPort = 78` | `A2StationDbServer` |
| `kMotherPort = 90` | `MothershipServer` |
| `kServerApiKey = 7b93a32d...d917d2` | `MothershipServer.SERVER_API_KEY` — **identical**, the two were built for each other |

Set `kBackendHost` / `kMotherHost` in the DLL to wherever you run this.

## Secrets worth changing

All hardcoded, all in `src\AUnrealFeatures.AAMothership\MothershipServer.cs` unless noted:

- `SERVER_API_KEY` — shared with the game-server DLL. Change both together or the DLL's
  quest fetch and the `/v1/server/authorized` join gate stop working.
- `JWT_SECRET` — signs player tokens.
- `ENVIRONMENT_ID`, `TENANT_ID` — echoed back to the client.
- the `production` database password in `src\AUnrealFeatures.Ares\Program.cs`.

## What was removed, and what had to stay

Dropped entirely: **PlayFab**, **Virtex/Skybox**, **ContentDownloadNetwork**, the unit-test
projects, build output, logs, committed `.db` files, and the `.rar`/`.7z` archives. Also
dropped `AUnrealFeatures.Ares\Content\` (9 MB of partner-portal templates) — nothing in the
kept code references it.

**EOSSDK** and **HalcyonSocket** are still referenced, because the two servers we keep read
static state from them:

- `A2StationDbServer` → `EosGatewayServer.UpsertSession` / `.Sessions` (an in-memory list
  of registered servers) and `HalcyonSocketServer.PendingNames` (a name handed over by the
  dashboard's "Spin Up" button)
- `AresDashboardServer` → `EosObservabilityStore.GetTelemetry`

Those are static stores, so they work with the servers switched off — the data just is not
served over HTTP. `Program.cs` has both as commented one-liners if you want them:

- EOS gateway `:50` + websocket `:80` — the fake Epic Online Services API, needed if the
  client discovers servers through EOS matchmaking instead of the station API.
- Halcyon socket `:9095` — the control channel the dashboard uses to launch game servers.

Verified before trimming: nothing outside `Program.cs` referenced PlayFab, Virtex, Skybox
or ContentDownloadNetwork, and all 8 remaining project references resolve.

## Status

Assembled and structurally checked — project references resolve, the trim is
reference-clean, no build output or databases came along. **Not yet compiled**: this
machine has no .NET SDK and no Node, so `dotnet build` and `npm install` have not run.
Expect to fix the odd small thing on the first build; the changes made here were confined
to `Program.cs`, `AUnrealFeatures.Ares.csproj` and `dashboard\next.config.ts`.

The original untouched repo is still at `Documents\AUnrealFeatures-main`, including a
prebuilt `AUnrealFeatures.Ares.exe` under `AUnrealFeatures.Ares\bin\Release\net6.0\` if you
want to run something before installing the SDK — that build starts every server, not just
these three.
