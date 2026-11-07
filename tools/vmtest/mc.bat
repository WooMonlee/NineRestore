@echo off
rem ============================================================================
rem  mc.bat - Windows guest diagnostics collector (test bench, NOT product code)
rem
rem  Usage (run inside the guest):
rem      mc.bat <round-tag>
rem  or  set MC_ROUND=<round-tag> ^&^& mc.bat
rem
rem  Exit codes:  0 = all steps ok
rem               1 = at least one step failed (details in the log)
rem               2 = bad invocation (round tag missing)
rem
rem  Why this script is written this way (PIT-218):
rem    * it used to redirect every line into C:\mc.txt and echo nothing, so a
rem      console window showed a blank screen for minutes ("looks hung");
rem    * it used to upload C:\mc.txt with -InFile while cmd still held the same
rem      file open for ">>" -> guaranteed sharing violation, swallowed by a bare
rem      catch{}, so the upload silently never happened and exit code stayed 0;
rem    * it had no -TimeoutSec, so an unreachable collector stalled 21 s per call.
rem    Fix: snapshot the log before uploading, require the round tag, log every
rem    failure with its reason, and return a real exit code.
rem
rem  Keep this file pure ASCII: a UTF-8 non-ASCII comment line makes cmd treat
rem  part of it as a command when the console code page differs.
rem ============================================================================
setlocal EnableExtensions

set "RC=0"
set "ROUND=%~1"
if not defined ROUND set "ROUND=%MC_ROUND%"
if not defined ROUND set "ROUND=%R%"
if not defined ROUND goto :badargs

if not defined U set "U=http://10.99.99.1:8901"

set "WORK=%~dp0work"
if not exist "%WORK%" mkdir "%WORK%" >nul 2>&1
set "L=%WORK%\mc_%ROUND%.txt"
set "UP=%WORK%\mc_%ROUND%.up.txt"
set "VD=%WORK%\vd_%ROUND%.txt"

echo [mc] round=%ROUND% collector=%U%
echo [mc] collecting into %L%

echo === ROUND INFO %DATE% %TIME% === > "%L%"
echo --- host --- >> "%L%"
ver >> "%L%" 2>&1
if exist "%SystemRoot%\System32\wbem\wmic.exe" (
  wmic os get Caption,Version,BuildNumber,InstallDate,OSArchitecture /value >> "%L%" 2>&1
) else (
  echo [mc] WARN: wmic not present on this build >> "%L%"
)
echo --- setup --- >> "%L%"
reg query HKLM\SYSTEM\Setup /v SetupType >> "%L%" 2>&1
reg query HKLM\SYSTEM\Setup /v SystemSetupInProgress >> "%L%" 2>&1
reg query HKLM\SYSTEM\Setup /v OOBEInProgress >> "%L%" 2>&1
reg query HKLM\SYSTEM\Setup\Status >> "%L%" 2>&1
echo --- crashcontrol --- >> "%L%"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\CrashControl" >> "%L%" 2>&1
echo --- dump --- >> "%L%"
dir C:\Windows\Minidump >> "%L%" 2>&1
dir C:\Windows\MEMORY.DMP >> "%L%" 2>&1
echo --- old/new --- >> "%L%"
dir C:\~old >> "%L%" 2>&1
dir C:\~new >> "%L%" 2>&1
echo --- events 1001 --- >> "%L%"
wevtutil qe System /q:"*[System[(EventID=1001)]]" /f:text /c:5 >> "%L%" 2>&1
echo --- events 41 --- >> "%L%"
wevtutil qe System /q:"*[System[(EventID=41)]]" /f:text /c:5 >> "%L%" 2>&1
echo --- events 6008 --- >> "%L%"
wevtutil qe System /q:"*[System[(EventID=6008)]]" /f:text /c:5 >> "%L%" 2>&1
echo --- events 6005/6009 --- >> "%L%"
wevtutil qe System /q:"*[System[(EventID=6005 or EventID=6009)]]" /f:text /c:8 >> "%L%" 2>&1
echo --- panther --- >> "%L%"
dir C:\Windows\Panther >> "%L%" 2>&1
echo --- bcd --- >> "%L%"
bcdedit /enum {default} >> "%L%" 2>&1
echo --- disk --- >> "%L%"
echo select disk 0 > "%VD%"
echo detail disk >> "%VD%"
echo select partition 1 >> "%VD%"
echo detail partition >> "%VD%"
diskpart /s "%VD%" >> "%L%" 2>&1
if errorlevel 1 (
  echo [mc] WARN: diskpart failed, continuing >> "%L%"
  set "RC=1"
)

echo [mc] uploading...
rem Snapshot first: never upload a file that cmd still has open for append.
copy /y "%L%" "%UP%" >nul 2>&1
call :put "%UP%" "%U%/mc_%ROUND%.txt" "mc log"
call :put "%VD%" "%U%/vd_%ROUND%.txt" "disk detail"
call :put "C:\Windows\Panther\setupact.log" "%U%/setupact_%ROUND%.log" "setupact"
call :put "C:\Windows\Panther\setuperr.log" "%U%/setuperr_%ROUND%.log" "setuperr"

echo === DONE %TIME% rc=%RC% === >> "%UP%"
copy /y "%UP%" "%USERPROFILE%\Desktop\mc_%ROUND%.txt" >nul 2>&1

if not "%RC%"=="0" (
  echo [mc] FAILED: some steps did not complete, see %UP%
  goto :finish
)
echo [mc] OK: %UP%

:finish
if "%MC_PAUSE%"=="1" pause
endlocal & exit /b %RC%

:put
rem %1 = local file, %2 = url, %3 = label
if not exist "%~1" (
  echo [mc] SKIP %~3: local file missing ^(%~1^)>> "%UP%"
  set "RC=1"
  goto :eof
)
powershell -NoProfile -Command "try{Invoke-WebRequest -Uri '%~2' -Method Put -InFile '%~1' -UseBasicParsing -TimeoutSec 10|Out-Null; Write-Output 'PUT-OK: %~3'}catch{Write-Output ('PUT-FAIL: %~3 : '+$_.Exception.Message); exit 1}" >> "%L%" 2>&1
if errorlevel 1 (
  echo [mc] FAILED upload %~3 -^> %~2
  set "RC=1"
  goto :eof
)
echo [mc]   ok %~3
goto :eof

:badargs
echo [mc] ERROR: round tag missing.
echo [mc]   usage: mc.bat ^<round-tag^>        e.g. mc.bat win11
echo [mc]   or:    set MC_ROUND=win11 ^&^& mc.bat
endlocal & exit /b 2
