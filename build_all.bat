@echo off
rem Build both architectures, stage output\, and compile the installer.
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
pushd "%ROOT%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found. Install Visual Studio 2022 Build Tools.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo ERROR: no Visual Studio with the C++ toolset was found.
  exit /b 1
)
echo Visual Studio: %VSDIR%

where cmake >nul 2>&1
if errorlevel 1 (
  set "CMAKE=%ProgramFiles%\CMake\bin\cmake.exe"
) else (
  set "CMAKE=cmake"
)

echo.
echo === Building x86 ===
"%CMAKE%" -A Win32 -S . -B build_x86 || exit /b 1
"%CMAKE%" --build build_x86 --config Release || exit /b 1

echo.
echo === Building x64 ===
"%CMAKE%" -A x64 -S . -B build_x64 || exit /b 1
"%CMAKE%" --build build_x64 --config Release || exit /b 1

echo.
echo === Staging output\ ===
rem A helper left running by an earlier test holds output\mono_host.exe open.
taskkill /f /im mono_host.exe >nul 2>&1
if exist output rmdir /s /q output
mkdir output
mkdir output\x64
mkdir output\engine
xcopy /e /i /y /q engine output\engine >nul || exit /b 1
copy /y build_x86\Release\mono_host.exe            output\ >nul || exit /b 1
copy /y build_x86\Release\Monologue97Config.exe    output\ >nul || exit /b 1
copy /y build_x86\Release\monologue_sapi_x86.dll   output\ >nul || exit /b 1
copy /y build_x86\Release\sapi_test_x86.exe        output\ >nul
copy /y build_x64\Release\monologue_sapi_x64.dll   output\x64\ >nul || exit /b 1
copy /y build_x64\Release\sapi_test_x64.exe        output\ >nul

echo.
echo === Building the installer ===
set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
  echo WARNING: Inno Setup 6 not found; skipping the installer.
) else (
  if not exist dist mkdir dist
  "%ISCC%" installer\Monologue97SAPI.iss || exit /b 1
)

echo.
echo Done.
popd
endlocal
