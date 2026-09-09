@echo off
REM ---------------------------------------------------------------------------------------
REM  Start-MockPlayer.bat - one-click MOCK PLAYER for debugging.
REM
REM  Launches PlayerNovBuild (windowed, real rendering) with the current UE4SS mods
REM  (A2ConsoleUnlock + A2EntitlementPatch + A2PlayerControl) and AUTO-CONNECTS to the
REM  running LAN station. Pair it with Start-Spectator.bat to watch replication from a
REM  second window.
REM
REM  Controls:  F1 leave spectator | WASD fly | Space/Ctrl up/down | Shift boost
REM             F2 toggle WASD | `  UE console
REM  Mod log:   %TEMP%\A2PlayerControl.log
REM
REM  Usage:
REM    Start-MockPlayer.bat                     connect to 192.168.1.29:7777 (the LAN station)
REM    Start-MockPlayer.bat 127.0.0.1:7777      connect to a different server
REM ---------------------------------------------------------------------------------------
setlocal
set "GAME=%~dp0..\..\PlayerNovBuild\A2\Binaries\Win64\A2-Win64-Shipping.exe"
if not exist "%GAME%" (
    echo PlayerNovBuild not found. Build it first:
    echo     powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0New-TestClients.ps1"
    pause
    exit /b 1
)

REM Default to the LAN station registered on the backend; override with arg 1.
set "TARGET=192.168.1.29:7777"
if not "%~1"=="" set "TARGET=%~1"

echo Starting mock player -> %TARGET%
REM A2PlayerControl parses -connectToServerByIPAndPort and calls the game's own
REM UA2SessionSubsystem::DirectConnectToServerByIPAndPort once the session subsystem is up.
start "A2 MOCK PLAYER" "%GAME%" -connectToServerByIPAndPort=%TARGET% -windowed -ResX=1280 -ResY=720 -WinX=0 -WinY=0 -log
