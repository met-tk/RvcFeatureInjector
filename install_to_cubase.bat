@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\Install-UserPlugin.ps1" -PluginBundle "%~dp0dist\RvcFeatureInjector.vst3"
if errorlevel 1 exit /b 1
pause