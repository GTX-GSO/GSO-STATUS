@echo off
cd /d "%~dp0"
start "" "%~dp0artifacts\GSW\GSW.exe" --config "%~dp0config\gsw-nas.json"
start "" "%~dp0artifacts\GSS-Simulator.exe" --config "%~dp0config\display-nas.json"
