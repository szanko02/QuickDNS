@echo off
setlocal
cd /d "%~dp0"

if not "%~1"=="" goto mingw

:: Prefer Visual Studio's bundled CMake, which supports its installed version.
set "CMAKE=cmake.exe"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%I\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "CMAKE=%%I\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
)

set "BUILD_DIR=build\msvc"
set "APP_EXE=%BUILD_DIR%\Release\QuickDNS.exe"
"%CMAKE%" -S . -B "%BUILD_DIR%" -A x64
if errorlevel 1 exit /b 1
goto build

:mingw
set "CMAKE=cmake.exe"
set "PATH=%~f1;%PATH%"
set "CC="
set "CXX="
set "RC="
if exist "%~f1\gcc.exe" set "CC=%~f1\gcc.exe"
if exist "%~f1\g++.exe" set "CXX=%~f1\g++.exe"
if exist "%~f1\clang.exe" set "CC=%~f1\clang.exe"
if exist "%~f1\clang++.exe" set "CXX=%~f1\clang++.exe"
if exist "%~f1\windres.exe" set "RC=%~f1\windres.exe"
if exist "%~f1\llvm-windres.exe" set "RC=%~f1\llvm-windres.exe"
if not defined CC goto missing_toolchain
if not defined CXX goto missing_toolchain
if not defined RC goto missing_toolchain
set "BUILD_DIR=build\mingw"
set "APP_EXE=%BUILD_DIR%\QuickDNS.exe"
"%CMAKE%" -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=%CC%" "-DCMAKE_CXX_COMPILER=%CXX%" "-DCMAKE_RC_COMPILER=%RC%"
if errorlevel 1 exit /b 1

:build
"%CMAKE%" --build "%BUILD_DIR%" --config Release --parallel 4
if errorlevel 1 exit /b 1
copy /y "%APP_EXE%" QuickDNS.exe >nul
if errorlevel 1 exit /b 1
echo Built %CD%\QuickDNS.exe
exit /b 0

:missing_toolchain
echo [ERROR] Expected a C and C++ compiler and windres or llvm-windres in "%~f1".
exit /b 1
