# Rigel Rift (PC) publish build

The PC twin of the Quest client, packaged to look and behave like an ordinary shipping build: a stock
A2 PC build (launcher + `Engine\` + `A2\`, same layout as `specnovbuild`), pointed at our backend,
initialised under our own Meta app, with **no UE4SS and no mods**. Zip the output folder and hand it
over, or use it as the payload of the published Rift app.

Build it (or rebuild after a new stock drop):

```
cd RigelRift\eosredirect
.\build.ps1                                       # builds the EOS redirect dsound.dll (once)
cd ..
python make_rift.py --src ..\specnovbuild --dst ..\RiftPublish     # copy + strip + patch + config + dsound
python make_rift.py --dst ..\RiftPublish --verify                 # re-check any time
```

## What is changed, and why each one is done that way

| what | where | how |
|---|---|---|
| Mod tooling | `A2\Binaries\Win64` | **removed** — UE4SS (`dwmapi.dll` proxy, `ue4ss\`, `Mods`), IGCS (`IGCS_ImGuiSettings.ini`), and any `*.log`. A publish build carries none of it. |
| Dashboard / station API | constant in `A2-Win64-Shipping.exe` | patched **in place**: `https://api.oriondrift.net` → `https://rigel.wwiggles.org` (both 26 chars) |
| Mothership (login, user data) | `A2\Saved\Config\Windows\Engine.ini` | config override merged over the packaged `DefaultEngine.ini` |
| Oculus app identity | same config override | `RiftAppId` → `1366120006579163` (our "spec"/publish Meta app) |
| EOS station browser | `A2\Binaries\Win64\dsound.dll` | a game binary that redirects EOS at runtime — see below |

**The dashboard URL is length-locked** (patched in place), which is why the station host is `rigel`
(26 chars, same as `https://api.oriondrift.net`).

**The Mothership URL** can't use that trick (ours is longer), so it rides the user config, which UE
merges over the packaged defaults.

## Why the entitlement check passes without a patch

The build is initialised under **our own Meta app** (`RiftAppId = 1366120006579163`). The signed-in
account owns that app, so the Oculus platform grants the entitlement for real and the check passes on
its own — nothing is bypassed. Keep `RIFT_APP_ID` in step with `MothershipServer.RIFT_APP_ID`: the
backend verifies the client's UserProof nonce under that app (and its federated app
`1641990017505530`), so they must match or logins fail. `RIFT_ENFORCE` is `false` (capture-first:
verify + log, still issue the session) until real Rift logins show up in the backend log.

## The EOS redirect (`eosredirect\`)

The PC EOS SDK (`EOSSDK-Win64-Shipping.dll`) builds its backend host at runtime
(`EpicGamesPlatform::GetBaseURL`) and talks to it through a **statically-linked libcurl** — so, exactly
like the Quest `.so`, there is no config key and no import to repoint. The station browser is reached by
`/matchmaking/v1/.../filter` on that backend, so without a redirect a Rift player logs in but sees no
stations.

`eosredirect\dsound.dll` fixes that while looking like a game binary:

- **Disguise.** `A2-Win64-Shipping.exe` statically imports `dsound.dll` by ordinal (1, 3, 6, 8, 11, 12),
  and `dsound` is not a KnownDLL, so our copy next to the exe loads first. `thunks.asm` forwards every
  ordinal to the real `C:\Windows\System32\dsound.dll`, so audio is unchanged. It is just a DLL the game
  already loads, used as a place to run at start-up — no console, no log window, no extra folder. By
  default it writes nothing at all; set `RIGEL_EOS_DIAG=1` before launching to get a one-line-per-event
  `rigel_eos.diag` in `%TEMP%` for a single validation run.
- **Redirect.** At start-up it waits for the EOS SDK, finds `curl_easy_setopt` by a 24-byte signature
  (the varargs shim that spills rdx/r8/r9 and tail-calls the internal option setter; confirmed at RVA
  `0xcfaf90` in the shipped SDK, re-found by scan if a rebuild moves it), and hooks it. For
  `CURLOPT_URL`, any `epicgames.dev` URL is rewritten to `https://rigel-eos.wwiggles.org`, scheme
  included, path preserved. Every other option and every non-Epic URL passes through untouched. This is
  the PC twin of `Rigel\hook\rigel_hook.cpp`.

Source: `eosredirect\dllmain.cpp`, `thunks.asm`, `exports.def`, `build.ps1`. The built `dsound.dll` is
checked in so `make_rift.py` can place it without the C++ toolchain; rebuild it if you change the
source or the EOS SDK version changes.

## Server side

The Mothership carries `RIFT_APP_ID = 1366120006579163` and its federated app, and verifies a Rift
login's UserProof nonce against those (plus the Quest app). `RIFT_ENFORCE` is `false` for now.

## What is not in git

`RiftPublish\` is the ~3 GB build and is ignored, along with `eosredirect\build\` intermediates. Only
the tooling, the redirect source, and the small built `dsound.dll` are tracked, so a fresh checkout
rebuilds the folder from a stock build with one command.
