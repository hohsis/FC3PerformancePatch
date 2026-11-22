@echo off
rem FC3PerformancePatch - Windows installer (double-click me).
rem Options are passed through to install.ps1, for example:
rem     install.bat -EnableDXVK
rem     install.bat -GameDir "D:\Games\Far Cry 3"
setlocal
rem Started from PowerShell 7, the inherited module path breaks Windows PowerShell.
set "PSModulePath="
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
set "RC=%ERRORLEVEL%"
echo.
if not "%FC3PP_NO_PAUSE%"=="1" pause
exit /b %RC%
