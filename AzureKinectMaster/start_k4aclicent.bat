@echo off
:restart
echo Starting k4arecorder...
AzureKinectMaster
echo AzureKinectMaster closed. Restarting in 60 seconds...
timeout /t 60 /nobreak
goto restart
