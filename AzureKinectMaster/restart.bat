@echo off
:loop
start /wait "" "..\x64\Release\AzureKinectMaster"
echo 程序已退出，5 秒后重新启动...
timeout /t 5 /nobreak
goto loop
