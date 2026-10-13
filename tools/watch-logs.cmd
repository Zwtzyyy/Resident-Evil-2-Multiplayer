@echo off
REM re2r_mp live log watcher (double-click or run from a terminal).
REM Pass -All to see every line.  Example:  watch-logs.cmd -All
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0watch-logs.ps1" %*
pause
