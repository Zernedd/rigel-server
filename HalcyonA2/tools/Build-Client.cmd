@echo off
REM Double-click this to build a redirected A2 client. It just asks questions.
REM (Same thing as: python build_client.py)
setlocal
cd /d "%~dp0"

where python >nul 2>&1
if errorlevel 1 (
    echo Python was not found on PATH. Install 64-bit Python 3.9+ and try again.
    pause
    exit /b 1
)

python build_client.py %*
echo.
pause
