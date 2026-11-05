@echo off
rem 方案 C（引擎内输入）构建脚本。与 build_all.bat 分开，避免污染默认发布构建。
rem 依赖本机已有的 UE4SS 源码树 + 预编译 import lib（见 CMakeLists 注释）。
setlocal
set "PF86=%ProgramFiles(x86)%"
if "%PF86%"=="" set "PF86=C:\Program Files (x86)"
set "VSROOT=%PF86%\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [engine-build] VS 2022 BuildTools not found
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

set "UE4SS_SRC=%PBZFM_UE4SS_DIR%"
if "%UE4SS_SRC%"=="" set "UE4SS_SRC=D:\Documents\DSH\PBZ\_ref\ue4ss-src"
set "UE4SS_LIB=%PBZFM_UE4SS_LIBDIR%"
if "%UE4SS_LIB%"=="" set "UE4SS_LIB=D:\Documents\DSH\PBZ\PBZPhotoModeCpp\build-dll\Game__Shipping__Win64\lib"
set "UE4SS_GEN=%PBZFM_UE4SS_GENERATED_DIR%"
if "%UE4SS_GEN%"=="" set "UE4SS_GEN=D:\Documents\DSH\PBZ\PBZPhotoModeCpp\build-dll\ue4ss\UE4SS\generated"

cmake -S . -B build_engine -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DPBZFM_TEST_HOOKS=OFF ^
      -DPBZFM_ENGINE_PORT=ON ^
      -DPBZFM_UE4SS_DIR="%UE4SS_SRC%" ^
      -DPBZFM_UE4SS_LIBDIR="%UE4SS_LIB%" ^
      -DPBZFM_UE4SS_GENERATED_DIR="%UE4SS_GEN%" || exit /b 1
cmake --build build_engine || exit /b 1
echo [engine-build] OK
