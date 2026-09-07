@echo off
setlocal EnableExtensions

set "PROJECT_ROOT=%~dp0"
set "BIN_DIR=%PROJECT_ROOT%assets\bin"
set "APP_BIN=%PROJECT_ROOT%build\Tiny-Drone.bin"
set "OUTPUT_BIN=%PROJECT_ROOT%flash_all.bin"

if not exist "%APP_BIN%" set "APP_BIN=%PROJECT_ROOT%tiny-drone-build\build\Tiny-Drone.bin"

if not exist "%APP_BIN%" (
    echo [ERROR] Tiny-Drone.bin not found. Build the firmware first.
    exit /b 1
)

for %%F in (bootloader.bin partition-table.bin nvs.bin ota_data_initial.bin) do (
    if not exist "%BIN_DIR%\%%F" (
        echo [ERROR] Missing %BIN_DIR%\%%F
        exit /b 1
    )
)

if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"
copy /y "%APP_BIN%" "%BIN_DIR%\Tiny-Drone.bin" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy Tiny-Drone.bin to assets\bin.
    exit /b 1
)

set "IDF_PYTHON="
if defined IDF_PYTHON_ENV_PATH if exist "%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" (
    set "IDF_PYTHON=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe"
)
if not defined IDF_PYTHON if defined IDF_TOOLS_PATH (
    for /d %%D in ("%IDF_TOOLS_PATH%\python_env\idf*_py*_env") do (
        if exist "%%~fD\Scripts\python.exe" set "IDF_PYTHON=%%~fD\Scripts\python.exe"
    )
)
if not defined IDF_PYTHON (
    for /f "delims=" %%P in ('where python.exe 2^>nul') do if not defined IDF_PYTHON set "IDF_PYTHON=%%P"
)
if not defined IDF_PYTHON (
    echo [ERROR] ESP-IDF Python was not found. Run this script from an ESP-IDF environment.
    exit /b 1
)

echo Merging flash_all.bin...
"%IDF_PYTHON%" -m esptool --chip esp32s3 merge_bin -o "%OUTPUT_BIN%" -f raw ^
    0x0 "%BIN_DIR%\bootloader.bin" ^
    0x8000 "%BIN_DIR%\partition-table.bin" ^
    0x9000 "%BIN_DIR%\nvs.bin" ^
    0xe000 "%BIN_DIR%\ota_data_initial.bin" ^
    0x10000 "%BIN_DIR%\Tiny-Drone.bin"
if errorlevel 1 (
    echo [ERROR] flash_all.bin merge failed.
    exit /b 1
)

echo [OK] %OUTPUT_BIN%
exit /b 0
