@echo off
REM Starts the Next.js dashboard UI on http://localhost:3000
REM Needs Node.js -> https://nodejs.org  (first run installs packages)
setlocal
cd /d "%~dp0dashboard"

where npm >nul 2>&1
if errorlevel 1 (
    echo npm was not found on PATH. Install Node.js: https://nodejs.org
    pause
    exit /b 1
)

if not exist node_modules (
    echo Installing dashboard packages, this happens once...
    call npm install || goto :fail
)

REM Point the UI at your backend; defaults to localhost.
if "%BACKEND_HOST%"=="" set BACKEND_HOST=127.0.0.1
echo Backend: %BACKEND_HOST%  (set BACKEND_HOST to change)
call npm run dev
goto :eof

:fail
echo npm install failed.
pause
