@echo off
rem ============================================================
rem  Whale Desktop Pet - internal launcher
rem  Builds a self-contained environment, then runs the Node host
rem  on the bundled Electron binary (ELECTRON_RUN_AS_NODE=1),
rem  so the target machine needs no system-wide Node.js install.
rem ============================================================
setlocal
set "BASE=%~dp0"
if "%BASE:~-1%"=="\" set "BASE=%BASE:~0,-1%"

set "DSH_HOME=%BASE%\userdata"
set "DSH_PET_PACKAGE_ROOT=%BASE%\dsh-pet"
set "DSH_PET_ELECTRON_PATH=%BASE%\electron\electron.exe"
set "DSH_PET_AUTOFIX_DSH=0"
set "ELECTRON_RUN_AS_NODE=1"

if not exist "%BASE%\userdata" mkdir "%BASE%\userdata" >nul 2>&1
if not exist "%BASE%\logs" mkdir "%BASE%\logs" >nul 2>&1

if not exist "%BASE%\electron\electron.exe" (
  echo [ERROR] electron\electron.exe is missing. Please re-extract the package.
  pause
  exit /b 1
)
if not exist "%BASE%\dsh-pet\package.json" (
  echo [ERROR] dsh-pet\ is missing. Please re-extract the package.
  pause
  exit /b 1
)

start "" "%BASE%\electron\electron.exe" "%BASE%\server.mjs"
exit /b 0
