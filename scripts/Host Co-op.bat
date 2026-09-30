@echo off
py -3 "%~dp0coop-launch.py" host %*
if errorlevel 1 pause
