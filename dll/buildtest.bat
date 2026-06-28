@echo off
setlocal
set MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\MSVC\14.29.30133
set SDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0
set INCLUDE=%MSVC%\include;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared;%SDK%\Include\%SDKVER%\um
set LIB=%MSVC%\lib\x86;%SDK%\Lib\%SDKVER%\ucrt\x86;%SDK%\Lib\%SDKVER%\um\x86
set PATH=%MSVC%\bin\Hostx86\x86;%PATH%
cd /d "%~dp0"
cl /nologo /O2 /MT /EHsc loadtest.cpp /link /OUT:loadtest.exe /MACHINE:X86 /SUBSYSTEM:CONSOLE
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD OK
endlocal
