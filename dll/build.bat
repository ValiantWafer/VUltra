@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 (echo vcvars failed & exit /b 1)
cd /d "%~dp0"
del /q openal32.dll openal32.lib openal32.exp dllmain.obj 2>nul
cl /nologo /LD /O2 /MT /EHsc /DNDEBUG dllmain.cpp /link /DEF:proxy.def /OUT:openal32.dll
if errorlevel 1 (echo BUILD FAILED & exit /b 1)
echo BUILD OK
dir openal32.dll
endlocal
