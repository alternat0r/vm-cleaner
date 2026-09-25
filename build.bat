@echo off
setlocal EnableDelayedExpansion

set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSDIR%" set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\Community"

REM ---- auto-increment version.h (minor up, build reset to 0) ----
set "VF=vm-cleaner\src\version.h"
if not exist "%VF%" (
  > "%VF%" echo #pragma once
  >> "%VF%" echo #define VERSION_MAJOR 1
  >> "%VF%" echo #define VERSION_MINOR 0
  >> "%VF%" echo #define VERSION_BUILD 0
  >> "%VF%" echo #define VERSION_STRING L"1.0.0"
)

set "MAJ=1"
set "MIN=0"
set "BLD=0"
for /f "tokens=3" %%a in ('findstr "VERSION_MAJOR" "%VF%"') do set "MAJ=%%a"
for /f "tokens=3" %%a in ('findstr "VERSION_MINOR" "%VF%"') do set "MIN=%%a"
for /f "tokens=3" %%a in ('findstr "VERSION_BUILD" "%VF%"') do set "BLD=%%a"

set /a MIN+=1
set "BLD=0"

> "%VF%" echo #pragma once
>> "%VF%" echo #define VERSION_MAJOR %MAJ%
>> "%VF%" echo #define VERSION_MINOR %MIN%
>> "%VF%" echo #define VERSION_BUILD %BLD%
>> "%VF%" echo #define VERSION_STRING L"%MAJ%.%MIN%.%BLD%"

echo [build.bat] version %MAJ%.%MIN%.%BLD%

"%VSDIR%\MSBuild\Current\Bin\MSBuild.exe" vm-cleaner.sln /p:Configuration=Release /p:Platform=x64 /nologo /m /v:m
if errorlevel 1 (
  exit /b %errorlevel%
)

REM ---- strip intermediate debug artifacts (keep only the final exe) ----
set "INT=vm-cleaner\x64\Release"
if exist "%INT%" (
  del /q "%INT%\*.obj"     "%INT%\*.iobj"    "%INT%\*.pdb"  "%INT%\*.ipdb"  "%INT%\*.ilk"   "%INT%\*.idb"  "%INT%\*.res"  "%INT%\*.recipe" "%INT%\*.lastbuildstate" 2>nul
  if exist "%INT%\vm-cleaner.tlog" rmdir /s /q "%INT%\vm-cleaner.tlog" 2>nul
)
echo [build.bat] stripped intermediate debug artifacts from %INT%
exit /b 0

