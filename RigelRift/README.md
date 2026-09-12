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
| Entitlement check | `A2-Win64-Shipping.exe` | one byte, `jne` → `jmp` |

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

## The entitlement byte

A client that is not the store build cannot prove the Meta account owns the app, and a Shipping
build treats that as fatal:

```
LogA2MothershipAuthStateMachine: Error: Could not verify entitlement status:
  Missing entitlement for ... A Shipping build would exit at this point
```

The guard in this build (22284):

```
145429424  test bpl, bpl    40 84 ED
145429427  jne  ...         75 59      <- entitled path
145429429  ...                         <- logs the error above, then exits
```

`make_rift.py --entitlement` makes that jump unconditional (`75` → `EB`) after checking the 5-byte
signature, and refuses to write if it does not match. This is the same change as the UE4SS mod in
`HalcyonA2/ue4ss-mods/A2EntitlementPatch`, which targets build 20996 and refuses on anything else —
baking it into the exe means the spec build needs no UE4SS at all.

Until this is applied the client exits a few seconds after login, so **the build is not testable
without it**.

## Server side is already done

The Mothership already knows about Rift: it carries `RIFT_APP_ID` and verifies a UserProof nonce
against the Quest credentials first and then the Rift ones, so a Rift login validates under the Rift
app. `RIFT_ENFORCE` is currently `false` — capture-first, meaning it verifies and logs but still
issues the session. Once real Rift logins show up in the backend log, turn it on.

## What is not in git

`A2\` is the game build (~2.8 GB) and is ignored. Only the tooling is tracked, so a fresh checkout
rebuilds the folder from a stock build with one command.
