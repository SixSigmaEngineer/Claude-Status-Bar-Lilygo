@echo off
rem Claude Status Bar bridge - run in a console window
cd /d "%~dp0"
where python >nul 2>nul || (echo Python not found. Install from https://python.org & pause & exit /b 1)
python -c "import serial" 2>nul || (echo Installing pyserial... & python -m pip install pyserial)
python claude_bar_bridge.py %*
pause
