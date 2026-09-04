@echo off
setlocal
set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul
pushd "%~dp0"
cl /nologo /W3 /O2 /EHsc /DWIN32 /D_CRT_SECURE_NO_WARNINGS render.cpp /Fe:render.exe /link user32.lib advapi32.lib winmm.lib
set RC=%errorlevel%
popd
exit /b %RC%
