AUTOMATED CRASH REPORT -- from RigelCrashWatch on this PC (HalcyonA2/tools/crashwatch). Nobody is watching this
session live; the user attaches later with `claude attach`. Work autonomously and safely.

A live Rigel game server crashed. Evidence bundle: {BUNDLE}
Crash signature: {SIGNATURE}   (server pid {PID}, exited {TIME} VPS time, exit code {CODE})

Your job: find the crash point and fix it.

1. Read the bundle. The FIRST fault block for the pid in its last seconds is usually the one that killed it; later ones
   are the cascade. Many fault blocks are first-chance faults our own SEH already handles (e.g. GAME +0xFD4425 with
   faultAddr -1) -- don't chase those.
2. Name the faulting code: GAME rvas -> IDA headless (scratchpad idaq.py, see memory a2-22284-luau-api-gaps / the
   IDA MCP notes); SELF rvas -> HalcyonA2\build\22284\x64\Release\HalcyonA2.map of the deployed build. Work out the
   chain: our payload code, game code reached through our payload (a call we make, an object we touch), or
   engine-internal damage (e.g. heap corruption detected in unrelated code -- see FIELD_NOTES "Live server crash
   2026-09-25").
3. Fix it in the HalcyonA2 source (or the backend / editor if that is where it is). Build. Reproduce and verify on the
   local test server (scratchpad gm_session.ps1 / full_setup.sh harness, mock player) wherever the crash can be
   reproduced; say plainly when it can't be.
4. Log it in SpecEditor/luau/mcp/FIELD_NOTES.md (what crashed, cause, fix, how verified), commit and push to main.
5. Deploy: stage the DLL on the VPS as C:/Env/rigel-server/windows/HalcyonA2.dll.new. Swap it in
   (C:/Env/vps_swap_now.ps1) ONLY if C:/Env/vps_players.ps1 reports 0 players online. With players on, leave it
   staged -- never restart a server with players on it.
6. If you can't find the cause or a safe fix, don't guess and don't deploy: write what you found to FIELD_NOTES and stop.
7. Finish with a PushNotification (one or two sentences: the crash, the cause, the fix, deployed or staged) and a short
   summary as your last message.

Rules that still apply (see memory): cap every wait; never kill the user's own Rigel editor (Meta Horizon install);
don't delete anything on the VPS; don't restart the backend unless it is itself the problem; patch C++ through files
written with Write/Edit, not heredocs.
