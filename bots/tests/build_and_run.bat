@echo off
rem Builds the standalone BotNav test with the VS2022 x86 toolchain and runs it on a .nav file.
rem Usage: build_and_run.bat <file.nav> [x y z ...]
setlocal
set VCVARS="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
call %VCVARS% >nul || exit /b 2
pushd "%~dp0"
if not exist _build mkdir _build
cl /nologo /EHsc /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /Fe_build\navtest.exe /Fo_build\ navtest.cpp ..\BotNav.cpp || (popd & exit /b 2)
_build\navtest.exe %*
set RC=%ERRORLEVEL%
popd
exit /b %RC%
