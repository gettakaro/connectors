@echo off
REM Start the DayZ ↔ Takaro bridge. Runs hidden so it doesn't steal focus.
cd /d "%~dp0"
start "DayZ Takaro Bridge" /B node dist\index.js
