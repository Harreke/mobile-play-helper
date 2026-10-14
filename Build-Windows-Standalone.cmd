@echo off
setlocal
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo Python 3 is required on the build computer.
  pause
  exit /b 1
)
python scripts\package_windows.py
if errorlevel 1 (
  echo Build failed. See the error above.
  pause
  exit /b 1
)
echo Done: packages\MobilePlayHelper-Windows-x64.exe
start "" explorer.exe "%~dp0packages"
pause
