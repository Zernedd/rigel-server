@echo off
REM ---------------------------------------------------------------------------------------
REM  Starts the PLAYER test client: PlayerNovBuild, a real windowed (non-headless) client
REM  with UE4SS + A2ConsoleUnlock + A2EntitlementPatch + A2PlayerControl.
REM
REM    F1          leave spectator (Server_ExitSpectator)
REM    WASD        fly around - hold to accelerate, release to coast
REM    Space/Ctrl  up / down
REM    Shift       boost
REM    F2          toggle the WASD system off/on
REM    `           UE console (A2ConsoleUnlock)
REM
REM  Usage:
REM    Start-Player.bat                    launch, then connect yourself with:  open 127.0.0.1:7777
REM    Start-Player.bat 127.0.0.1:7777     connect on launch
REM
REM  -nohmd runs it flat on the desktop with real rendering, so replication is actually visible.
REM  Mod log: %TEMP%\A2PlayerControl.log
REM ---------------------------------------------------------------------------------------
setlocal
set "GAME=%~dp0..\..\PlayerNovBuild\A2\Binaries\Win64\A2-Win64-Shipping.exe"

if not exist "%GAME%" (
    echo PlayerNovBuild not found. Build it first:
    echo     powershell -NoProfile -File "%~dp0New-TestClients.ps1"
    pause
    exit /b 1
)

REM A first argument connects on launch. The PLAYER build uses A2PlayerControl's
REM -connectToServerByIPAndPort switch, which calls the game's own
REM UA2SessionSubsystem::DirectConnectToServerByIPAndPort - the engine-level bare URL is
REM ignored by this game's frontend, so it is not used here.
set "CONNECT="
if not "%~1"=="" set "CONNECT=-connectToServerByIPAndPort=%~1"

start "A2 PLAYER" "%GAME%" %CONNECT% -windowed -ResX=1280 -ResY=720 -WinX=0 -WinY=0 -log
