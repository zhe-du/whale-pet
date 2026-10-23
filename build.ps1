# ============================================================================
# build.ps1 - Build the "Whale Desktop Pet standalone host" (C++)
#
# Toolchain : D:\LE\AI\tools\mingw64  (x86_64-posix-seh GCC 16.2)
# Output    : bin\whale-pet-host.exe  (GUI subsystem, no console, embedded icon)
#
# Usage: powershell -ExecutionPolicy Bypass -File build.ps1
# ============================================================================
param(
  [string]$Mingw = 'D:\LE\AI\tools\mingw64\bin'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin = Join-Path $root 'bin'
New-Item -ItemType Directory -Force -Path $bin | Out-Null

$gpp = Join-Path $Mingw 'g++.exe'
$windres = Join-Path $Mingw 'windres.exe'
if (-not (Test-Path $gpp)) { throw "g++.exe not found: $gpp" }
if (-not (Test-Path $windres)) { throw "windres.exe not found: $windres" }

$res = Join-Path $bin 'app.res.o'
Write-Host '[1/2] Compiling resources (icon / version info)...'
& $windres (Join-Path $root 'src\app.rc') -O coff -o $res
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $res)) { throw "windres failed (exit=$LASTEXITCODE)" }

Write-Host '[2/2] Compiling host executable...'
$exe = Join-Path $bin 'whale-pet-host.exe'
& $gpp `
  -std=c++17 -O2 -Wall -Wextra `
  -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX `
  -municode -mwindows `
  -o $exe `
  (Join-Path $root 'src\whale-pet-host.cpp') `
  $res `
  -lwinhttp -lws2_32 -lole32 -loleaut32 -lshell32 -lshlwapi -luuid `
  -static-libgcc -static-libstdc++ -static
if ($LASTEXITCODE -ne 0) { throw "g++ failed (exit=$LASTEXITCODE)" }

$info = Get-Item $exe
Write-Host ''
Write-Host ("BUILD OK: {0} ({1:N0} bytes)" -f $info.FullName, $info.Length) -ForegroundColor Green
