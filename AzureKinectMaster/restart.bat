@echo off
:loop
start /wait "" "..\x64\Release\AzureKinectMaster"
echo restart after 20 second
timeout /t 20 /nobreak
goto loop
