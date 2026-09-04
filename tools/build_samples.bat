@echo off
setlocal
set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
call "%VCVARS%" >nul
pushd "%~dp0"
cl /nologo /W3 /O2 /EHsc /std:c++17 /DWIN32 /D_CRT_SECURE_NO_WARNINGS make_samples.cpp ..\src\mono_core.cpp /Fe:make_samples.exe /link user32.lib advapi32.lib
set RC=%errorlevel%
popd
exit /b %RC%
