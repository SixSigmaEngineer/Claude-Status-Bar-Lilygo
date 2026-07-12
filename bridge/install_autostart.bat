@echo off
rem Makes the bridge start automatically (hidden window) when you log into Windows.
cd /d "%~dp0"
where pythonw >nul 2>nul || (echo Python not found. Install from https://python.org & pause & exit /b 1)
python -m pip install pyserial >nul 2>nul

set SHORTCUT=%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\ClaudeStatusBar.lnk
powershell -NoProfile -Command ^
  "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut('%SHORTCUT%');" ^
  "$s.TargetPath = (Get-Command pythonw).Source; $s.Arguments = '\"%~dp0claude_bar_bridge.py\"';" ^
  "$s.WorkingDirectory = '%~dp0'; $s.Save()"

if exist "%SHORTCUT%" (
    echo Installed! The bridge will auto-start at login.
    echo Starting it now...
    start "" pythonw "%~dp0claude_bar_bridge.py"
) else (
    echo Failed to create the startup shortcut.
)
pause
