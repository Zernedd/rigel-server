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
3. Fix it in the HalcyonA2 source (or the backend / editor if that is where it is). Build.
4. TEST IT LOCALLY -- required, no exceptions: run
     powershell -ExecutionPolicy Bypass -File HalcyonA2\tools\localtest\LocalSmoke.ps1 [-Repro <your repro script>]
   It runs your build on a local server with a mock player on a MiniJakeball team, a kickoff and a goal, a stock
   player joining, and a level reload, then checks no game process died and no crash report appeared (~10 min).
   Write a repro for THIS crash where you can (a .py using lt.py's helpers, or a .ps1; print FAIL on failure) and
   pass it with -Repro -- run it once on the OLD build to see it fail, then on the fixed build to see it pass. If the
   crash can't be reproduced locally, say so plainly. The smoke run must print "LOCAL SMOKE: PASS" before step 6.
5. Log it in SpecEditor/luau/mcp/FIELD_NOTES.md (what crashed, cause, fix, how verified -- paste the smoke result),
   commit and push to main.
6. Deploy (only after a local PASS): stage the DLL on the VPS as C:/Env/rigel-server/windows/HalcyonA2.dll.new. Swap
   it in (C:/Env/vps_swap_now.ps1) ONLY if C:/Env/vps_players.ps1 reports 0 players online. With players on, leave it
   staged -- never restart a server with players on it.
7. If you can't find the cause or a safe fix -- or the local test fails -- don't guess and don't deploy: write what you found to FIELD_NOTES and stop.
8. Finish with a PushNotification (one or two sentences: the crash, the cause, the fix, local test result, deployed or staged) and a short
   summary as your last message.

Rules that still apply (see memory): cap every wait; never kill the user's own Rigel editor (Meta Horizon install);
don't delete anything on the VPS; don't restart the backend unless it is itself the problem; patch C++ through files
written with Write/Edit, not heredocs.
