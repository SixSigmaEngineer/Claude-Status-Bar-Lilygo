@echo off
rem Push this project to GitHub. First run sets everything up; after that,
rem double-click to commit + push all changes.
setlocal
cd /d "%~dp0"

where git >nul 2>nul || (echo Git not found. Install from https://git-scm.com & pause & exit /b 1)

if not exist ".git" (
    echo First run - initializing repository...
    git init
    git branch -M main
    git remote add origin https://github.com/SixSigmaEngineer/Claude-Status-Bar-Lilygo.git
)

git add -A
set MSG=%*
if "%MSG%"=="" set MSG=Update %date% %time%
git commit -m "%MSG%"
git push -u origin main
if errorlevel 1 (
    echo.
    echo Push failed. If this is the first push you may need to sign in,
    echo or if the remote has commits you don't: git pull --rebase origin main
)
pause
