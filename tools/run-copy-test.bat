@echo off
setlocal
rem Runs a copy with the normal Windows progress dialog and prints what the probe saw.
rem Needs the file-operation-probe mod enabled in Windhawk. Uses only a few MB of disk.

set "BASE=C:\Users\Public"
set "SRC=%BASE%\copytest-src"
set "DST=%BASE%\copytest-dst"
set "LOG=%BASE%\file-operation-probe.log"

echo Preparing test files...
if exist "%DST%" rmdir /S /Q "%DST%"
if exist "%SRC%" rmdir /S /Q "%SRC%"
mkdir "%SRC%" || goto :fail
rem 8000 files of 32 KB (about 256 MB). Many files copy slowly, so the dialog stays up
rem long enough to show real progress, not just the preparing phase.
fsutil file createnew "%SRC%\seed.dat" 32768 >nul || goto :fail
for /L %%i in (1,1,8000) do (copy /Y "%SRC%\seed.dat" "%SRC%\f%%i.dat" >nul)
del "%SRC%\seed.dat"

if exist "%LOG%" del "%LOG%"

echo Starting copy - a progress dialog should appear. Do not click it.
cscript //nologo "%~dp0copy-dialog.vbs"

echo.
echo ===== probe log =====
if exist "%LOG%" (type "%LOG%") else (echo No log file found - is the probe mod enabled?)
echo ===== end of log =====

echo.
echo Cleaning up test files...
rmdir /S /Q "%SRC%"
rmdir /S /Q "%DST%"
echo Done. Paste the log above into the chat.
endlocal
exit /b 0

:fail
echo Setup failed - could not create the test folder or files. Try again.
endlocal
exit /b 1
