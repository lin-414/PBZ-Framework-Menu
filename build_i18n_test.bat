@echo off
set "PF86=%ProgramFiles(x86)%"
if "%PF86%"=="" set "PF86=C:\Program Files (x86)"
call "%PF86%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /std:c++20 /utf-8 /EHsc /MT /I src /I third_party /Fe:build\i18n_test.exe tests\i18n_test.cpp /link /SUBSYSTEM:CONSOLE