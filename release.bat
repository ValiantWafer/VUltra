@echo off
setlocal enabledelayedexpansion
rem ===========================================================================
rem  V Ultra - one-click release builder
rem  Rebuilds the self-contained manager exe and repackages the release zip.
rem
rem  What it does:
rem    1. Builds the manager as a self-contained single-file win-x64 exe
rem       (bundles .NET so end users do NOT need any runtime installed).
rem    2. Refreshes the BUILT artifacts inside the release folder:
rem         - VUltraManager.exe        (the freshly built manager)
rem         - Manual-install\vultramod_proxy.dll   (current proxy DLL)
rem         - Manual-install\vultramod.ini         (current config template)
rem       The curated text files (README.txt, install.bat, uninstall.bat) are
rem       left untouched - edit those by hand when the release notes change.
rem    3. Zips the whole VUltra-v<VERSION> folder into VUltra-v<VERSION>.zip.
rem
rem  NOTE: this does NOT rebuild the native proxy DLL. If you changed the C++
rem  (dll\dllmain.cpp), run dll\build2.bat first - this script warns if the
rem  built DLL looks older than the source.
rem ===========================================================================

rem ---- bump this when you cut a new release ----
set "VERSION=1.4"

set "ROOT=%~dp0"
set "MANAGER=%ROOT%manager"
set "DLLDIR=%ROOT%dll"
set "RELDIR=%ROOT%_local\release\VUltra-v%VERSION%"
set "MANUAL=%RELDIR%\Manual-install"
set "ZIP=%ROOT%_local\release\VUltra-v%VERSION%.zip"

echo.
echo ==========================================================
echo   V Ultra release builder  -  v%VERSION%
echo ==========================================================

rem ---- warn if the native DLL looks stale vs its source ----
powershell -NoProfile -Command ^
  "$src=Get-Item '%DLLDIR%\dllmain.cpp' -ErrorAction SilentlyContinue;" ^
  "$dll=Get-Item '%DLLDIR%\openal32.dll' -ErrorAction SilentlyContinue;" ^
  "if($src -and $dll -and $src.LastWriteTime -gt $dll.LastWriteTime){" ^
  "  Write-Host '  WARNING: dll\dllmain.cpp is newer than dll\openal32.dll.' -ForegroundColor Yellow;" ^
  "  Write-Host '           Run dll\build2.bat first if the native mod changed.' -ForegroundColor Yellow }"

echo.
echo === [1/4] Building self-contained manager exe (large, please wait) ===
dotnet publish "%MANAGER%\VUltraManager.csproj" -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o "%MANAGER%\publish-sc"
if errorlevel 1 ( echo. & echo  BUILD FAILED. & pause & exit /b 1 )

echo.
echo === [2/4] Refreshing built artifacts in release folder ===
if not exist "%MANUAL%" mkdir "%MANUAL%"
copy /y "%MANAGER%\publish-sc\VUltraManager.exe" "%RELDIR%\VUltraManager.exe" >nul || ( echo  Could not copy exe & pause & exit /b 1 )
copy /y "%DLLDIR%\openal32.dll"  "%MANUAL%\vultramod_proxy.dll" >nul || ( echo  Could not copy proxy dll & pause & exit /b 1 )
copy /y "%DLLDIR%\vultramod.ini" "%MANUAL%\vultramod.ini"       >nul || ( echo  Could not copy ini & pause & exit /b 1 )
echo   exe, proxy dll, and ini updated.

echo.
echo === [3/4] Checking required files are present ===
set "MISSING="
for %%F in (
  "%RELDIR%\VUltraManager.exe"
  "%RELDIR%\README.txt"
  "%MANUAL%\vultramod_proxy.dll"
  "%MANUAL%\vultramod.ini"
  "%MANUAL%\install.bat"
  "%MANUAL%\uninstall.bat"
  "%MANUAL%\README.txt"
) do (
  if not exist "%%~F" ( echo   MISSING: %%~F & set "MISSING=1" )
)
if defined MISSING ( echo. & echo  Missing files above - not zipping. & pause & exit /b 1 )
echo   all present.

echo.
echo === [4/4] Zipping -^> %ZIP% ===
if exist "%ZIP%" del /q "%ZIP%"
powershell -NoProfile -Command "Compress-Archive -Path '%RELDIR%' -DestinationPath '%ZIP%' -Force"
if errorlevel 1 ( echo. & echo  ZIP FAILED. & pause & exit /b 1 )

echo.
echo ==========================================================
echo   DONE.
echo   Folder: %RELDIR%
echo   Zip:    %ZIP%
echo ==========================================================
echo.
pause
endlocal
