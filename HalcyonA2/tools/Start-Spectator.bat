@echo off
REM ---------------------------------------------------------------------------------------
REM  Starts the SPECTATOR test client: specnovbuild, a real windowed (non-headless) client
REM  with UE4SS + A2ConsoleUnlock (+ A2EntitlementPatch, which it needs or it quits itself
REM  ~30 s after launch on a machine with no Meta entitlement).
REM
REM  Deliberately has NO A2PlayerControl: this one just watches, so whatever it sees moving
REM  really did come over the wire from the server.
REM
REM    `   UE console (A2ConsoleUnlock) - e.g.  open 127.0.0.1:7777
REM
REM  Usage:
REM    Start-Spectator.bat                    launch, then connect yourself from the console
REM    Start-Spectator.bat 127.0.0.1:7777     connect on launch
REM ---------------------------------------------------------------------------------------
setlocal
set "GAME=%~dp0..\..\specnovbuild\A2\Binaries\Win64\A2-Win64-Shipping.exe"

if not exist "%GAME%" (
    echo specnovbuild not found. Build it first:
    echo     powershell -NoProfile -File "%~dp0New-TestClients.ps1"
    pause
    exit /b 1
)

REM NOTE: this build has NO A2PlayerControl (console mod only, by design), so there is
REM nothing here to implement -connectToServerByIPAndPort - that switch does not exist in
REM the shipping binary and would be ignored. Connect this one from the ` console:
REM     open 127.0.0.1:7777
REM The argument is still forwarded in case you want the engine to see it.
set "CONNECT="
if not "%~1"=="" set "CONNECT=%~1"

REM Offset to the right of the player window so both are visible side by side.
start "A2 SPECTATOR" "%GAME%" %CONNECT% -nohmd -windowed -ResX=1280 -ResY=720 -WinX=1290 -WinY=0 -log
