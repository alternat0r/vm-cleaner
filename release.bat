@echo off
setlocal EnableDelayedExpansion

REM release.bat - build, commit+push, and publish a GitHub release.
REM   release.bat            full release (commit + push + gh release)
REM   release.bat /dryrun    build + report version only (no push, no release)

set "DRY=0"
if "%~1"=="/dryrun" set "DRY=1"

echo [release.bat] Building (auto-increments minor version)...
call build.bat
if errorlevel 1 (
  echo [release.bat] Build failed. Aborting release.
  exit /b 1
)

REM ---- read the new version from version.h ----
set "MAJ=1"
set "MIN=0"
set "BLD=0"
for /f "tokens=3" %%a in ('findstr "VERSION_MAJOR" "vm-cleaner\src\version.h"') do set "MAJ=%%a"
for /f "tokens=3" %%a in ('findstr "VERSION_MINOR" "vm-cleaner\src\version.h"') do set "MIN=%%a"
for /f "tokens=3" %%a in ('findstr "VERSION_BUILD" "vm-cleaner\src\version.h"') do set "BLD=%%a"
set "VER=%MAJ%.%MIN%.%BLD%"
set "TAG=v%VER%"
set "EXE=bin\x64\Release\vmcleaner.exe"

echo [release.bat] Releasing %VER% (tag %TAG%)

if not exist "%EXE%" (
  echo [release.bat] ERROR: %EXE% not found. Aborting.
  exit /b 1
)

if "%DRY%"=="1" (
  echo [release.bat] DRY RUN - skipping commit/push and GitHub release.
  endlocal
  exit /b 0
)

REM ---- commit + push source ----
call push.bat "Release v%VER%"
if errorlevel 1 (
  echo [release.bat] Commit/push failed. Aborting release.
  exit /b 1
)

REM ---- create the GitHub release with the exe as an asset ----
where gh >nul 2>nul
if errorlevel 1 (
  echo [release.bat] ERROR: gh CLI not found. Install it to create the release.
  exit /b 1
)

gh release create "%TAG%" "%EXE%" --title "VM Cleaner v%VER%" --notes "VM Cleaner v%VER%."
if errorlevel 1 (
  echo [release.bat] GitHub release creation failed.
  exit /b 1
)

echo [release.bat] Release %TAG% published successfully.
endlocal
exit /b 0
