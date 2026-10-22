@echo off
cd /d "%~dp0"
rem GSW installed by scripts\install-gsw.ps1 starts at sign-in; use it when present, otherwise the build in artifacts.
set "GSW_INSTALLED=%LOCALAPPDATA%\Programs\GSO STATUS WIN"
if exist "%GSW_INSTALLED%\GSW.exe" (
 start "" "%GSW_INSTALLED%\GSW.exe" --config "%GSW_INSTALLED%\gsw.json"
) else (
 start "" "%~dp0artifacts\GSW\GSW.exe" --config "%~dp0config\gsw-nas.json"
)
start "" "%~dp0artifacts\GSS-Simulator.exe" --config "%~dp0config\display-nas.json"
