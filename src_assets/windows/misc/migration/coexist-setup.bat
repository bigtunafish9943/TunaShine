@echo off
setlocal

rem Lets TunaStream be installed next to Sunshine or Apollo.
rem All three use the same network ports by default, so only one could run at a time.
rem If one of the others is installed and no port has been chosen yet, give TunaStream
rem its own port range and a distinct name so both can run and be told apart.

for %%I in ("%~dp0\..") do set "ROOT_DIR=%%~fI"
set "CONF_DIR=%ROOT_DIR%\config"
set "CONF=%CONF_DIR%\tunastream.conf"
set "TS_PORT=48989"

set "OTHER="
sc query SunshineService >nul 2>&1
if not errorlevel 1 set "OTHER=Sunshine"
sc query ApolloService >nul 2>&1
if not errorlevel 1 set "OTHER=Apollo"

if not defined OTHER (
    echo No other streaming host found, keeping the default port.
    exit /b 0
)

if not exist "%CONF_DIR%\" mkdir "%CONF_DIR%"

set "HAS_PORT="
set "HAS_NAME="
if exist "%CONF%" (
    findstr /r /b /i /c:"port *=" "%CONF%" >nul 2>&1
    if not errorlevel 1 set "HAS_PORT=1"
    findstr /r /b /i /c:"tunastream_name *=" "%CONF%" >nul 2>&1
    if not errorlevel 1 set "HAS_NAME=1"
)

if defined HAS_PORT (
    echo %OTHER% is installed, but a port is already set. Leaving the configuration alone.
    exit /b 0
)

echo %OTHER% is installed. Setting TunaStream to port %TS_PORT% so both can run.
rem Start on a new line in case the file does not end with one
>>"%CONF%" echo.
>>"%CONF%" echo port = %TS_PORT%
if not defined HAS_NAME >>"%CONF%" echo tunastream_name = %COMPUTERNAME%-TunaStream

exit /b 0
