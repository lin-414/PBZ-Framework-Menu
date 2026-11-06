@echo off
set "PF86=%ProgramFiles(x86)%"
if "%PF86%"=="" set "PF86=C:\Program Files (x86)"
set "VSROOT=%PF86%\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [build] VS 2022 BuildTools not found
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
where cmake ninja cl 2>nul
rem PBZFM_TEST_HOOKS=ON: the host regression drives the DLL through its test
rem exports; release packaging must reconfigure with OFF (tools/pack.ps1 warns).
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPBZFM_TEST_HOOKS=ON || exit /b 1
cmake --build build || exit /b 1
echo [build] OK