@echo off
setlocal
set MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC\14.29.30133
set SDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

set INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um
set LIB=%MSVC%\lib\x86;%SDK%\Lib\%SDKVER%\ucrt\x86;%SDK%\Lib\%SDKVER%\um\x86
set PATH=%MSVC%\bin\Hostx86\x86;%PATH%

cd /d "%~dp0"
del /q openal32.dll openal32.lib openal32.exp dllmain.obj 2>nul
cl /nologo /LD /O2 /MT /EHsc /DNDEBUG dllmain.cpp /link /OUT:openal32.dll /MACHINE:X86
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD OK
endlocal
