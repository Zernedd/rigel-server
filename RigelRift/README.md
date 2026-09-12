# Rigel Rift (PC) spec build

The PC twin of the Quest client. A stock A2 PC build, copied into its own folder and pointed at our
backend, so a Rift player can log in and join our stations. Zip `A2\` and hand it over, or use it as
the payload of the published Rift app.

Build it (or rebuild after a new stock drop):

```
cd RigelRift
python make_rift.py --src ..\Nov15\A2 --dst A2     # copy + redirect
python make_rift.py --dst A2 --entitlement         # the one byte, see below
python make_rift.py --dst A2 --verify              # re-check any time
```

## What is changed, and why each one is done that way

| what | where | how |
|---|---|---|
| Dashboard / station API | compile-time constant in `A2-Win64-Shipping.exe` | patched **in place**: `https://api.oriondrift.net` → `https://rigel.wwiggles.org` |
| Mothership (login, user data) | `A2/Config/DefaultEngine.ini` inside `A2-Windows.pak` | **not** patched — set in `A2/Saved/Config/Windows/Engine.ini` instead |
| EOS gateway | same | same config override |
| Oculus app identity | same packaged ini | `RiftAppId` → our own Meta app, via the same config override |

**The dashboard URL is length-locked.** It is overwritten in place, so the replacement must be the
same byte length as `https://api.oriondrift.net` — 26 characters. `https://rigel.wwiggles.org` is
exactly 26. Nothing longer can ever go here; that is why the station host is `rigel` and not
something more descriptive.

**The Mothership URL cannot use the same trick.** The packaged string is
`https://aa-mothership.com` (25) and ours is `https://rigel-ms.wwiggles.org` (29). An in-place pak
patch cannot change a string's length — the entry's uncompressed size has to stay fixed — and the
alternative (inventing a shorter host) would mean a new non-`rigel-*` DNS record, which we do not
do. So it goes in the user config, which UE merges *over* the packaged defaults. No length limit,
and it can be changed later without touching the pak at all.

## Why no entitlement patch is needed

The build is initialised under **our own Meta app** (`RiftAppId = 2240289680101933`, the "rigel"
app) instead of the stock one. The signed-in account owns that app, so the Oculus platform grants
the entitlement for real and the check passes on its own — nothing is bypassed. The Quest build
works the same way via `MobileAppId`.

With the stock app id the account cannot prove it owns the store title under this build, and a
Shipping build exits a few seconds after login:

```
LogA2MothershipAuthStateMachine: Error: Could not verify entitlement status:
  Missing entitlement for ... A Shipping build would exit at this point
```

`make_rift.py --entitlement` still exists — it flips the one-byte guard in the exe, the same change
as the UE4SS mod in `HalcyonA2/ue4ss-mods/A2EntitlementPatch` — but it is **not needed and not
recommended** now that the app id is ours. Keep it for diagnosis only.

Keep `RIFT_APP_ID` here in step with `MothershipServer.RIFT_APP_ID`: the backend verifies the
UserProof nonce under that app, so they must be the same app or logins fail.

## Server side is already done

The Mothership already knows about Rift: it carries `RIFT_APP_ID` and verifies a UserProof nonce
against the Quest credentials first and then the Rift ones, so a Rift login validates under the Rift
app. `RIFT_ENFORCE` is currently `false` — capture-first, meaning it verifies and logs but still
issues the session. Once real Rift logins show up in the backend log, turn it on.

## What is not in git

`A2\` is the game build (~2.8 GB) and is ignored. Only the tooling is tracked, so a fresh checkout
rebuilds the folder from a stock build with one command.
