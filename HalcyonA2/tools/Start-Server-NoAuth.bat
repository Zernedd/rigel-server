@echo off
REM ---------------------------------------------------------------------------------------
REM  Starts the HalcyonA2 server with the join AUTH GATE DISABLED (testing only).
REM
REM  The payload normally kicks any client that has not completed the dashboard/mothership
REM  auth handshake, about 30 seconds after it joins ("[GATE] KICK no-auth-timeout").
REM  server.noauth.psd1 passes -NoAuthGate, which makes that gate log-only so the windowed
REM  test clients can stay connected. Do NOT use this profile for a real deployment.
REM
REM  Server listens on UDP 7777. Connect from a client with the ` console:
REM      open 127.0.0.1:7777
REM ---------------------------------------------------------------------------------------
setlocal
cd /d "%~dp0"

where powershell >nul 2>&1
if errorlevel 1 (
    echo PowerShell was not found on PATH.
    pause
    exit /b 1
)

echo [server] starting HalcyonA2 with auth gate DISABLED ...
powershell -NoProfile -ExecutionPolicy Bypass -File ".\Start-Server.ps1" -Config "server.noauth.psd1" %*
echo.
echo [server] stop it later with:  powershell -NoProfile -File .\Start-Server.ps1 -Stop
pause
