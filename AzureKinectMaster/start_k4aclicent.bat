@echo off

:Check
echo Checking AzureKinectMaster.exe processes...

:: 預設 count=0，避免沒有任何進程時變數未定義
set count=0

:: 使用 tasklist 查詢所有 AzureKinectMaster.exe，並計算符合條件的行數
for /f "tokens=1" %%C in ('tasklist /FI "IMAGENAME eq AzureKinectMaster.exe" ^| find /I "AzureKinectMaster.exe" ^| find /C /V ""') do (
    set count=%%C
)

echo Found %count% instance(s) of AzureKinectMaster.exe.

:: 若 >= 2，代表已經有 2 或更多實例在跑，就不動作
if %count% GEQ 2 (
    echo 2 or more instances are running. Doing nothing.
    echo Waiting 10 seconds before next check...
    timeout /t 10 /nobreak >nul
    goto Check
)

:: 若 = 1，代表目前只剩 1 個在跑，也不要動作，等它結束
if %count% EQU 1 (
    echo 1 instance is running. Doing nothing.
    echo Waiting 10 seconds before next check...
    timeout /t 10 /nobreak >nul
    goto Check
)

:: 若 = 0，就代表所有都結束了，可以一次啟動 2 個
if %count% EQU 0 (
    echo 0 instances found. Starting 2 new ones...
    start "" /B AzureKinectMaster.exe 192.168.50.200
    start "" /B AzureKinectMaster.exe 192.168.50.200
    echo Wait 5 seconds after launching...
    timeout /t 5 /nobreak >nul
    goto Check
)

goto Check
