@echo off
del "%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\ClaudeStatusBar.lnk" 2>nul
taskkill /f /im pythonw.exe 2>nul
echo Autostart removed (and any hidden bridge stopped).
pause
