@echo off
rem Runs setup-windows.ps1 without changing PowerShell's execution policy (Bypass applies to this run only).
rem Arguments are passed through, e.g.  tools\setup-windows.cmd -CheckOnly
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-windows.ps1" %*
exit /b %ERRORLEVEL%
