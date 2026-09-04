@echo off
rem Build the 32-bit Monologue engine probe.
setlocal
set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
  echo ERROR: vcvars32.bat not found at "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0"
cl /nologo /W3 /O2 /EHsc /DWIN32 /D_CRT_SECURE_NO_WARNINGS probe.cpp /Fe:probe.exe /link user32.lib advapi32.lib psapi.lib
set RC=%errorlevel%
popd
exit /b %RC%
