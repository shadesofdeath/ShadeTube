@echo off
rem Usage: build.bat [Debug|Release|RelWithDebInfo] [target] [builddir]
rem Finds Visual Studio (vswhere), enters the x64 dev environment, configures with Ninja and builds.
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Debug
set TARGET=%~2
set BUILDDIR=%~3
if "%BUILDDIR%"=="" set BUILDDIR=build\%CONFIG%

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if "%VSDIR%"=="" (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0"
if not exist "%BUILDDIR%\build.ninja" (
    cmake -S . -B "%BUILDDIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
)
if "%TARGET%"=="" (
    cmake --build "%BUILDDIR%" || exit /b 1
) else (
    cmake --build "%BUILDDIR%" --target %TARGET% || exit /b 1
)
endlocal
