@echo off
rem Lanza el instalador de PowerShell (doble clic). Acepta los mismos argumentos que instalar.ps1.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0instalar.ps1" %*
exit /b %errorlevel%
