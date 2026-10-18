@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build.ps1" -Test
if errorlevel 1 exit /b 1
if not exist "%~dp0work\qa" mkdir "%~dp0work\qa"
cd /d "%~dp0work\qa"
start /wait "" "%~dp0artifacts\GSS-Test.exe" --selftest
if errorlevel 1 (echo SELFTEST FAILED) else (type selftest.txt)
pause
