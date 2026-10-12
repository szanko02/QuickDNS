@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo             QuickDNS Windows Builder
echo ===================================================

:: 1. User-specified compiler bin via argument or env var
set "COMPILER_BIN="
if not "%~1"=="" (
    if exist "%~1\clang++.exe" set "COMPILER_BIN=%~1"
    if exist "%~1\g++.exe" set "COMPILER_BIN=%~1"
)

:: 2. Auto-detect from PATH
if "!COMPILER_BIN!"=="" (
    for /f "delims=" %%I in ('where clang++.exe 2^>nul') do (
        if "!COMPILER_BIN!"=="" set "COMPILER_BIN=%%~dpI"
    )
)
if "!COMPILER_BIN!"=="" (
    for /f "delims=" %%I in ('where g++.exe 2^>nul') do (
        if "!COMPILER_BIN!"=="" set "COMPILER_BIN=%%~dpI"
    )
)

:: 3. Auto-detect from WinGet MartinStorsjo.LLVM-MinGW package
if "!COMPILER_BIN!"=="" (
    if defined LOCALAPPDATA (
        for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW*") do (
            for /d %%B in ("%%D\llvm-mingw*\bin") do (
                if exist "%%B\clang++.exe" set "COMPILER_BIN=%%B"
            )
            if exist "%%D\bin\clang++.exe" set "COMPILER_BIN=%%D\bin"
        )
    )
)

:: 4. Auto-detect from standard toolchain paths
if "!COMPILER_BIN!"=="" if exist "C:\llvm-mingw\bin\clang++.exe" set "COMPILER_BIN=C:\llvm-mingw\bin"
if "!COMPILER_BIN!"=="" if exist "C:\msys64\ucrt64\bin\clang++.exe" set "COMPILER_BIN=C:\msys64\ucrt64\bin"
if "!COMPILER_BIN!"=="" if exist "C:\msys64\ucrt64\bin\g++.exe" set "COMPILER_BIN=C:\msys64\ucrt64\bin"
if "!COMPILER_BIN!"=="" if exist "C:\msys64\mingw64\bin\clang++.exe" set "COMPILER_BIN=C:\msys64\mingw64\bin"
if "!COMPILER_BIN!"=="" if exist "C:\msys64\mingw64\bin\g++.exe" set "COMPILER_BIN=C:\msys64\mingw64\bin"
if "!COMPILER_BIN!"=="" if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "COMPILER_BIN=%ProgramFiles%\LLVM\bin"

:: Trim trailing backslash if present
if "!COMPILER_BIN:~-1!"=="\" set "COMPILER_BIN=!COMPILER_BIN:~0,-1!"

:: 5. Validate compiler
set "CXX="
if not "!COMPILER_BIN!"=="" (
    if exist "!COMPILER_BIN!\clang++.exe" (
        set "CXX=!COMPILER_BIN!\clang++.exe"
    ) else if exist "!COMPILER_BIN!\g++.exe" (
        set "CXX=!COMPILER_BIN!\g++.exe"
    )
)

if "!CXX!"=="" (
    echo [ERROR] No C++20 compiler [clang++ or g++] was found on this system!
    echo.
    echo To build QuickDNS, please install LLVM-MinGW or MSYS2:
    echo   winget install MartinStorsjo.LLVM-MinGW.UCRT
    echo.
    echo Or run this script specifying the bin directory:
    echo   build.bat C:\path\to\llvm-mingw\bin
    echo ===================================================
    exit /b 1
)

:: 6. Locate Resource Compiler (windres / llvm-windres)
set "RC="
if exist "!COMPILER_BIN!\windres.exe" (
    set "RC=!COMPILER_BIN!\windres.exe"
) else if exist "!COMPILER_BIN!\llvm-windres.exe" (
    set "RC=!COMPILER_BIN!\llvm-windres.exe"
) else (
    for /f "delims=" %%I in ('where windres.exe 2^>nul') do if "!RC!"=="" set "RC=%%I"
    if "!RC!"=="" for /f "delims=" %%I in ('where llvm-windres.exe 2^>nul') do if "!RC!"=="" set "RC=%%I"
)

if "!RC!"=="" (
    echo [ERROR] Resource compiler [windres.exe or llvm-windres.exe] not found!
    exit /b 1
)

echo [INFO] Compiler: !CXX!
echo [INFO] Resource: !RC!
echo.

if not exist "build" mkdir build

echo [1/6] Compiling application icons and resources...
"!RC!" -I include -I res res\resource.rc -O coff -o build\resource.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] Resource compilation failed. & exit /b 1 )

"!RC!" -I include -I res res\resource_diag.rc -O coff -o build\resource_diag.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] Diag resource compilation failed. & exit /b 1 )

echo [2/6] Compiling DNS Network Engine and Benchmark...
"!CXX!" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I include -c src\dns_manager.cpp -o build\dns_manager.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] DNS manager compilation failed. & exit /b 1 )

"!CXX!" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I include -c src\dns_benchmark.cpp -o build\dns_benchmark.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] DNS benchmark compilation failed. & exit /b 1 )

echo [3/6] Compiling Config Manager...
"!CXX!" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I include -c src\config_manager.cpp -o build\config_manager.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] Config manager compilation failed. & exit /b 1 )

echo [4/6] Compiling Modern Win32 GUI, Benchmark and Dialogs...
"!CXX!" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I include -c src\dialog_add_profile.cpp -o build\dialog_add_profile.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] Add Profile Dialog compilation failed. & exit /b 1 )

"!CXX!" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I include -c src\dialog_benchmark.cpp -o build\dialog_benchmark.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] Benchmark Dialog compilation failed. & exit /b 1 )

"!CXX!" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I include -c src\ui_main.cpp -o build\ui_main.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] UI compilation failed. & exit /b 1 )

"!CXX!" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I include -c src\main.cpp -o build\main.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] Main compilation failed. & exit /b 1 )

echo [5/6] Linking QuickDNS.exe with embedded App Icon...
"!CXX!" -std=c++20 -O2 -mwindows -municode -static -o build\QuickDNS.exe build\main.o build\ui_main.o build\dialog_add_profile.o build\dialog_benchmark.o build\dns_benchmark.o build\dns_manager.o build\config_manager.o build\resource.o -liphlpapi -ldnsapi -lws2_32 -lcomctl32 -luxtheme -ldwmapi -lshell32 -lole32 -luuid -luser32 -lgdi32 -ladvapi32 -lgdiplus
if %ERRORLEVEL% neq 0 ( echo [ERROR] Linking QuickDNS.exe failed. & exit /b 1 )
copy /Y build\QuickDNS.exe "%~dp0QuickDNS.exe" >nul

echo [6/6] Linking Diagnostic tools [test_dns.exe, test_config.exe, test_benchmark.exe]...
"!CXX!" -std=c++20 -O2 -static -I include tests\test_dns.cpp build\dns_manager.o build\resource_diag.o -liphlpapi -ldnsapi -lws2_32 -luuid -lole32 -o build\test_dns.exe
if %ERRORLEVEL% neq 0 ( echo [ERROR] Linking test_dns.exe failed. & exit /b 1 )

"!CXX!" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -static -I include tests\test_config.cpp build\config_manager.o build\resource_diag.o -o build\test_config.exe
if %ERRORLEVEL% neq 0 ( echo [ERROR] Linking test_config.exe failed. & exit /b 1 )

"!CXX!" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -static -I include tests\test_benchmark.cpp build\dns_benchmark.o build\dns_manager.o build\config_manager.o build\resource_diag.o -liphlpapi -ldnsapi -lws2_32 -luuid -lole32 -o build\test_benchmark.exe
if %ERRORLEVEL% neq 0 ( echo [ERROR] Linking test_benchmark.exe failed. & exit /b 1 )

echo ===================================================
echo [SUCCESS] All executables built successfully!
echo  - Main App:   %~dp0QuickDNS.exe
echo  - Tests:      %~dp0build\test_dns.exe
echo                %~dp0build\test_config.exe
echo                %~dp0build\test_benchmark.exe
echo ===================================================
