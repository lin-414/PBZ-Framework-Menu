@echo off
rem Clean release build: no test exports (PBZFM_TEST_HOOKS=OFF), used for the A/B package.
setlocal
set "PF86=%ProgramFiles(x86)%"
if "%PF86%"=="" set "PF86=C:\Program Files (x86)"
set "VSROOT=%PF86%\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [release-build] VS 2022 BuildTools not found
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
cmake -S . -B build_release -G Ninja -DCMAKE_BUILD_TYPE=Release -DPBZFM_TEST_HOOKS=OFF || exit /b 1
cmake --build build_release || exit /b 1
echo [release-build] OK
