@echo off
cd /d "%~dp0artifacts"
start "" "GSS-Simulator.exe" --config "%~dp0config\display.json"
