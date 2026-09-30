@echo off
rem Builds dgmod. Usage: build.cmd [debug^|release^|arm64]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=release
set PRESET=x64-%CONFIG%
set VCVARS=vcvars64.bat
if /i "%CONFIG%"=="arm64" (set PRESET=arm64-release& set VCVARS=vcvarsamd64_arm64.bat)
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSDIR=%%i
if not exist "%VSDIR%\VC\Auxiliary\Build\%VCVARS%" (echo %VCVARS% not found: install the MSVC build tools for this target. & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist build\%PRESET%\build.ninja cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
