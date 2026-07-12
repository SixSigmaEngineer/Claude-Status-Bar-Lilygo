@echo off
rem Claude Status Bar - desktop app (bridge + live preview + tray)
cd /d "%~dp0"
where python >nul 2>nul || (echo Python not found. Install from https://python.org & pause & exit /b 1)
python -c "import serial, PIL, pystray" 2>nul || (echo Installing dependencies... & python -m pip install pyserial pillow pystray)
start "" pythonw claude_bar_app.py
