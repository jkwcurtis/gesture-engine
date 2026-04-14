@echo off
setlocal

:: Gesture Engine build script
:: Requires: GCC (MinGW-w64)

:: Check PATH first, then common WinGet location
where gcc >nul 2>&1
if %errorlevel% equ 0 (
    set "GCC=gcc"
    set "WINDRES=windres"
    goto :found
)

:: WinGet install location
for /f "delims=" %%G in ('dir /b /s "%LOCALAPPDATA%\Microsoft\WinGet\Packages\*gcc.exe" 2^>nul') do (
    set "GCC=%%G"
    set "WINDRES=%%~dpGwindres.exe"
    goto :found
)

echo ERROR: gcc not found.
echo Install: winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT
exit /b 1

:found
echo Using: %GCC%

:: Compile resource file (visual styles manifest + icon)
echo Compiling resources...
"%WINDRES%" src/gesture_engine.rc -o gesture_engine_res.o
if %errorlevel% neq 0 (
    echo Resource compile FAILED.
    exit /b 1
)

echo Building gesture_engine.exe ...
"%GCC%" -Os -Wall -Wextra -Wno-stringop-truncation -Wno-format-truncation ^
    -mwindows -municode ^
    -Isrc -Ivendor ^
    src/gesture_engine.c src/config.c src/actions.c src/settings_ui.c vendor/cJSON.c ^
    gesture_engine_res.o ^
    -o gesture_engine.exe ^
    -luser32 -lshell32 -lkernel32 -lcomctl32 -lcomdlg32 -lgdi32 -lpsapi -lversion -ladvapi32

if %errorlevel% equ 0 (
    del gesture_engine_res.o 2>nul
    echo Build successful: gesture_engine.exe
) else (
    del gesture_engine_res.o 2>nul
    echo Build FAILED.
    exit /b 1
)
