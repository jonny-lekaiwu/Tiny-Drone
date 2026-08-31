@echo off
setlocal

if not defined IDF_PATH (
    for /f "usebackq delims=" %%I in (`powershell.exe -NoProfile -Command "$e = Get-Content '%IDF_TOOLS_PATH%\idf-env.json' -Raw | ConvertFrom-Json; ($e.idfInstalled.PSObject.Properties.Value | Select-Object -First 1).path"`) do set "IDF_PATH=%%I"
)

if not exist "%IDF_PATH%\export.bat" (
    echo [ERROR] ESP-IDF export.bat not found. IDF_PATH=%IDF_PATH%
    exit /b 1
)

set "IDF_PYTHON_ENV_PATH=%IDF_TOOLS_PATH%\python_env\idf5.5_py3.11_env"
set "PATH=%IDF_PYTHON_ENV_PATH%\Scripts;%PATH%"
call "%IDF_PATH%\export.bat" >nul
if errorlevel 1 exit /b %errorlevel%

rem Some Windows IDF installations do not export CMake/Ninja even though the
rem tools are installed. Add their versioned directories without hardcoding a
rem particular tool version.
for /d %%D in ("%IDF_TOOLS_PATH%\tools\cmake\*") do set "PATH=%%~fD\bin;%PATH%"
for /d %%D in ("%IDF_TOOLS_PATH%\tools\ninja\*") do set "PATH=%%~fD;%PATH%"

"%IDF_TOOLS_PATH%\python_env\idf5.5_py3.11_env\Scripts\python.exe" "%~dp0scripts\build.py" %*
exit /b %errorlevel%
