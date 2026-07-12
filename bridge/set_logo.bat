@echo off
rem Set the display logo. Double-click for a file picker, or drag an image onto this file.
cd /d "%~dp0"
where python >nul 2>nul || (echo Python not found. Install from https://python.org & pause & exit /b 1)
python set_logo.py %*
pause
