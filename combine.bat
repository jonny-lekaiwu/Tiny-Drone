@echo off
setlocal EnableExtensions

set "PROJECT_ROOT=%~dp0"
set "BIN_DIR=%PROJECT_ROOT%assets\bin"
set "OUTPUT_BIN=%PROJECT_ROOT%flash_all.bin"

if not "%~1"=="" (
    set "APP_BIN=%~1\Tiny-Drone.bin"
) else (
    set "APP_BIN=%PROJECT_ROOT%build\Tiny-Drone.bin"
)

if not exist "%APP_BIN%" if "%~1"=="" set "APP_BIN=%PROJECT_ROOT%tiny-drone-build\build\Tiny-Drone.bin"

if not exist "%APP_BIN%" (
    echo [ERROR] Tiny-Drone.bin not found. Build the firmware first.
    exit /b 1
)

for %%F in ("%APP_BIN%") do set "BUILD_BIN_DIR=%%~dpF"
set "BOOT_BIN=%BUILD_BIN_DIR%bootloader\bootloader.bin"
set "PARTITION_BIN=%BUILD_BIN_DIR%partition_table\partition-table.bin"
set "OTA_BIN=%BUILD_BIN_DIR%ota_data_initial.bin"
set "NVS_BIN=%BIN_DIR%\nvs.bin"

for %%F in ("%BOOT_BIN%" "%PARTITION_BIN%" "%NVS_BIN%" "%OTA_BIN%") do (
    if not exist "%%~F" (
        echo [ERROR] Missing %%~F
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
    0x0 "%BOOT_BIN%" ^
    0x8000 "%PARTITION_BIN%" ^
    0x9000 "%NVS_BIN%" ^
    0xe000 "%OTA_BIN%" ^
    0x10000 "%BIN_DIR%\Tiny-Drone.bin"
if errorlevel 1 (
    echo [ERROR] flash_all.bin merge failed.
    exit /b 1
)

echo [OK] %OUTPUT_BIN%
exit /b 0
