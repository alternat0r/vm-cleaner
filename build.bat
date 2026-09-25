@echo off
setlocal EnableDelayedExpansion

set "VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSDIR%" set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\Community"

REM ---- auto-increment version.h ----
set "VF=vm-cleaner\src\version.h"
if not exist "%VF%" (
  > "%VF%" echo #pragma once
  >> "%VF%" echo #define VERSION_MAJOR 1
  >> "%VF%" echo #define VERSION_MINOR 0
  >> "%VF%" echo #define VERSION_BUILD 0
  >> "%VF%" echo #define VERSION_STRING L"1.0.0"
)
set /a B=0
for /f "tokens=3" %%i in ('type "%VF%" ^| findstr /C:"VERSION_BUILD"') do set /a B=%%i
set /a B+=1
> "%VF%" echo #pragma once
>> "%VF%" echo #define VERSION_MAJOR 1
>> "%VF%" echo #define VERSION_MINOR 0
>> "%VF%" echo #define VERSION_BUILD %B%
>> "%VF%" echo #define VERSION_STRING L"1.0.%B%"

"%VSDIR%\MSBuild\Current\Bin\MSBuild.exe" vm-cleaner.sln /p:Configuration=Release /p:Platform=x64 /nologo /m /v:m
exit /b %errorlevel%
