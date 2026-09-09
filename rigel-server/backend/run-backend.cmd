@echo off
REM Starts the backend: Mothership :90, station API :78, dashboard API :8080.
REM Needs the .NET 6 SDK -> https://dotnet.microsoft.com/download/dotnet/6.0
setlocal
cd /d "%~dp0"

where dotnet >nul 2>&1
if errorlevel 1 (
    echo The .NET SDK was not found on PATH.
    echo Install .NET 6: https://dotnet.microsoft.com/download/dotnet/6.0
    pause
    exit /b 1
)

REM Ports 78 and 90 are below 1024. On Windows that is fine for a normal user,
REM but if binding fails, run this from an elevated prompt.
dotnet run --project src\AUnrealFeatures.Ares -c Debug
pause
