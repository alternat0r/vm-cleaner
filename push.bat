@echo off
setlocal

if "%~1"=="" (
    call :log "Usage: push.bat \"commit message\""
    exit /b 1
)

set "COMMENT=%~1"

call :log "[1/3] Adding files..."
git add .

call :log "[2/3] Committing changes..."
git commit -m "%COMMENT%"

if errorlevel 1 (
    call :log "Commit failed or there are no changes to commit."
    exit /b 1
)

call :log "[3/3] Pushing to GitHub..."
git push

if errorlevel 1 (
    call :log "Push failed."
    exit /b 1
)

call :log "Successfully pushed to GitHub."
call :log "Commit: %COMMENT%"

endlocal
exit /b 0


:log
echo [%date% %time%] %~1
exit /b