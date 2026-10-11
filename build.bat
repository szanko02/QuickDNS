@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo             QuickDNS Windows Builder
echo ===================================================

set "COMPILER_BIN=C:\Users\SWEETY\AppData\Local\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\llvm-mingw-20260908-ucrt-x86_64\bin"

if not exist "!COMPILER_BIN!\clang++.exe" (
    echo [ERROR] Clang++ not found in expected path.
    exit /b 1
)

if not exist "build" mkdir build

echo [1/6] Compiling application icons and resources...
"!COMPILER_BIN!\windres.exe" -I include -I res res\resource.rc -O coff -o build\resource.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] Resource compilation failed. & exit /b 1 )

"!COMPILER_BIN!\windres.exe" -I include -I res res\resource_diag.rc -O coff -o build\resource_diag.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] Diag resource compilation failed. & exit /b 1 )

echo [2/6] Compiling DNS Network Engine...
"!COMPILER_BIN!\clang++.exe" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I include -c src\dns_manager.cpp -o build\dns_manager.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] DNS manager compilation failed. & exit /b 1 )

echo [3/6] Compiling Config Manager...
"!COMPILER_BIN!\clang++.exe" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I include -c src\config_manager.cpp -o build\config_manager.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] Config manager compilation failed. & exit /b 1 )

echo [4/6] Compiling Modern Win32 GUI and Dialogs...
"!COMPILER_BIN!\clang++.exe" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I include -c src\dialog_add_profile.cpp -o build\dialog_add_profile.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] Add Profile Dialog compilation failed. & exit /b 1 )

"!COMPILER_BIN!\clang++.exe" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I include -c src\ui_main.cpp -o build\ui_main.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] UI compilation failed. & exit /b 1 )

"!COMPILER_BIN!\clang++.exe" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN -I include -c src\main.cpp -o build\main.o
if %ERRORLEVEL% neq 0 ( echo [ERROR] Main compilation failed. & exit /b 1 )

echo [5/6] Linking QuickDNS.exe with embedded App Icon...
"!COMPILER_BIN!\clang++.exe" -std=c++20 -O2 -mwindows -municode -static -o build\QuickDNS.exe build\main.o build\ui_main.o build\dialog_add_profile.o build\dns_manager.o build\config_manager.o build\resource.o -liphlpapi -ldnsapi -lws2_32 -lcomctl32 -luxtheme -ldwmapi -lshell32 -lole32 -luuid -luser32 -lgdi32 -ladvapi32
if %ERRORLEVEL% neq 0 ( echo [ERROR] Linking QuickDNS.exe failed. & exit /b 1 )

echo [6/6] Linking Diagnostic tools (test_dns.exe, test_config.exe) with Diag Icons...
"!COMPILER_BIN!\clang++.exe" -std=c++20 -O2 -static -I include tests\test_dns.cpp build\dns_manager.o build\resource_diag.o -liphlpapi -ldnsapi -lws2_32 -luuid -lole32 -o build\test_dns.exe
"!COMPILER_BIN!\clang++.exe" -std=c++20 -O2 -finput-charset=utf-8 -fexec-charset=utf-8 -static -I include tests\test_config.cpp build\config_manager.o build\resource_diag.o -o build\test_config.exe

echo ===================================================
echo [SUCCESS] All executables built with custom icons!
echo  - Main App:   %~dp0build\QuickDNS.exe
echo  - Tests:      %~dp0build\test_dns.exe
echo                %~dp0build\test_config.exe
echo ===================================================
