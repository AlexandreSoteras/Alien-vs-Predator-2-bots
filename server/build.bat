@echo off
rem Rebuilds object.lto from scratch: Win32 Release, Visual Studio 2022.
rem   build.bat            wipe build\, configure, build, copy to out\
rem   build.bat fast       incremental build (keeps build\)
rem Output: out\object.lto (+ object.pdb, object.map)
setlocal
set "ROOT=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo vswhere.exe not found - is Visual Studio 2022 installed? & exit /b 1)
set "VSPATH="
rem run vswhere from its own directory: its path contains "(x86)", which breaks for /f
pushd "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq delims=" %%i in (`.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
popd
if not defined VSPATH (echo Visual Studio with the C++ tools was not found. & exit /b 1)
set "CMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" (echo CMake not found at "%CMAKE%" & exit /b 1)
set VSLANG=1033

if /i not "%~1"=="fast" if exist "%ROOT%build" rmdir /s /q "%ROOT%build"

"%CMAKE%" -S "%ROOT%." -B "%ROOT%build" -G "Visual Studio 17 2022" -A Win32 || exit /b 1
"%CMAKE%" --build "%ROOT%build" --config Release --target object -- -v:m -nologo || exit /b 1

if not exist "%ROOT%out" mkdir "%ROOT%out"
copy /y "%ROOT%build\Release\object.lto" "%ROOT%out\" >nul || exit /b 1
copy /y "%ROOT%build\Release\object.pdb" "%ROOT%out\" >nul
copy /y "%ROOT%build\Release\object.map" "%ROOT%out\" >nul
echo.
echo Built "%ROOT%out\object.lto"
for %%f in ("%ROOT%out\object.lto") do echo   %%~zf bytes
endlocal
