@echo off
cd /d "%~dp0"
wsl.exe bash tools/build-iso.sh
pause
