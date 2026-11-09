@echo off
cd /d "%~dp0"
wsl.exe env KNG_DEBIAN=1 bash tools/build-iso.sh
pause
