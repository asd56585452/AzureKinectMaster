@echo off

:Check
echo Checking AzureKinectMaster.exe processes...

:: �w�] count=0�A�קK�S������i�{���ܼƥ��w�q
set count=0

:: �ϥ� tasklist �d�ߩҦ� AzureKinectMaster.exe�A�íp��ŦX���󪺦��
for /f "tokens=1" %%C in ('tasklist /FI "IMAGENAME eq AzureKinectMaster.exe" ^| find /I "AzureKinectMaster.exe" ^| find /C /V ""') do (
    set count=%%C
)

echo Found %count% instance(s) of AzureKinectMaster.exe.

:: �Y >= 2�A�N��w�g�� 2 �Χ�h��Ҧb�]�A�N���ʧ@
if %count% GEQ 2 (
    echo 2 or more instances are running. Doing nothing.
    echo Waiting 10 seconds before next check...
    timeout /t 10 /nobreak >nul
    goto Check
)

:: �Y = 1�A�N��ثe�u�� 1 �Ӧb�]�A�]���n�ʧ@�A��������
if %count% EQU 1 (
    echo 1 instance is running. Doing nothing.
    echo Waiting 10 seconds before next check...
    timeout /t 10 /nobreak >nul
    goto Check
)

:: �Y = 0�A�N�N��Ҧ��������F�A�i�H�@���Ұ� 2 ��
if %count% EQU 0 (
    echo 0 instances found. Starting 2 new ones...
    start "" /B AzureKinectMaster.exe 192.168.50.200
    start "" /B AzureKinectMaster.exe 192.168.50.200
    echo Wait 5 seconds after launching...
    timeout /t 5 /nobreak >nul
    goto Check
)

goto Check
