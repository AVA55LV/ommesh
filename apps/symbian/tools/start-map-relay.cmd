@echo off
setlocal EnableExtensions DisableDelayedExpansion
title OMMesh map relay
if not exist "%~dp0tile_relay.py" goto missing_files
py.exe -3 -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>&1
if not errorlevel 1 goto use_launcher
python.exe -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>&1
if not errorlevel 1 goto use_python
echo Install Python 3.8 or newer from https://www.python.org/downloads/windows/
echo Enable the Python launcher or the Add Python to PATH option in the installer.
pause
exit /b 1

:use_launcher
py.exe -3 -X utf8 "%~dp0tile_relay.py" --interactive
if errorlevel 1 goto failed
exit /b 0

:use_python
python.exe -X utf8 "%~dp0tile_relay.py" --interactive
if errorlevel 1 goto failed
exit /b 0

:missing_files
echo Extract all files from the ZIP first. Keep this CMD file beside tile_relay.py.
goto failed

:failed
pause
exit /b 1
