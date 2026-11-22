@echo off
rem FC3PerformancePatch - Windows uninstaller (double-click me).
rem Removes the patch and restores every file it changed.
setlocal
rem Started from PowerShell 7, the inherited module path breaks Windows PowerShell.
set "PSModulePath="
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" %*
set "RC=%ERRORLEVEL%"
echo.
if not "%FC3PP_NO_PAUSE%"=="1" pause
exit /b %RC%
