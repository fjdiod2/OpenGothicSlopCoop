@echo off
py -3 "%~dp0coop-launch.py" join %*
if errorlevel 1 pause
