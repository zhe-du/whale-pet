@echo off
setlocal enabledelayedexpansion
set "BASE=%~dp0"
if "%BASE:~-1%"=="\" set "BASE=%BASE:~0,-1%"

set "KILLED=0"
set "PIDF=%BASE%\userdata\dsh-pet\standalone.pid"
if not exist "%PIDF%" set "PIDF=%BASE%\logs\standalone.pid"

if exist "%PIDF%" (
  set /p HPID=<"%PIDF%"
  if defined HPID (
    taskkill /PID !HPID! /T /F >nul 2>&1
    if not errorlevel 1 (
      echo [OK] host process !HPID! terminated
      set "KILLED=1"
    )
  )
)

if "!KILLED!"=="0" echo [INFO] no running host was found
echo.
echo Done. The pet disappears within a second or two.
timeout /t 2 >nul
