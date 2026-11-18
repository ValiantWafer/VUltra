@echo off
setlocal enabledelayedexpansion
rem ===========================================================================
rem  V Ultra - one-click release builder
rem  Rebuilds the self-contained manager exe and repackages the release zip.
rem
rem  What it does:
rem    1. Builds the manager as a self-contained single-file win-x64 exe
rem       (bundles .NET so end users do NOT need any runtime installed).
rem    2. Copies the freshly built VUltraManager.exe into the release folder.
rem       README.txt is left untouched - edit it by hand for each release.
rem       (The manager embeds the proxy DLL, so there's no manual install.)
rem    3. Zips the whole VUltra-v<VERSION> folder into VUltra-v<VERSION>.zip.
rem
rem  NOTE: this does NOT rebuild the native proxy DLL. If you changed the C++
rem  (dll\dllmain.cpp), run dll\build2.bat first - this script warns if the
rem  built DLL looks older than the source.
rem ===========================================================================

rem ---- bump this when you cut a new release ----
set "VERSION=1.5.5"

set "ROOT=%~dp0"
set "MANAGER=%ROOT%manager"
set "DLLDIR=%ROOT%dll"
set "RELDIR=%ROOT%_local\release\VUltra-v%VERSION%"
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

rem ---- refuse to ship the dev test harness ----
rem A plain dll\build2.bat includes it when _local\harness\testharness.h exists (on a dev
rem machine). Releases must be built with "dll\build2.bat release". The harness's script
rem file name only appears in the dll when the harness is compiled in.
for %%F in ("%DLLDIR%\openal32.dll" "%MANAGER%\VUltra.dll") do (
  if exist "%%~F" findstr /m /c:"vultra_test.txt" "%%~F" >nul && (
    echo.
    echo  %%~F contains the test harness.
    echo  Rebuild with "dll\build2.bat release" and copy it to manager\VUltra.dll, then rerun.
    pause & exit /b 1
  )
)

echo.
echo === [1/4] Building self-contained manager exe (large, please wait) ===
dotnet publish "%MANAGER%\VUltraManager.csproj" -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -o "%MANAGER%\publish-sc"
if errorlevel 1 ( echo. & echo  BUILD FAILED. & pause & exit /b 1 )

echo.
echo === [2/4] Copying the exe into the release folder ===
if not exist "%RELDIR%" mkdir "%RELDIR%"
copy /y "%MANAGER%\publish-sc\VUltraManager.exe" "%RELDIR%\VUltraManager.exe" >nul || ( echo  Could not copy exe & pause & exit /b 1 )
echo   exe updated.

echo.
echo === [3/4] Checking required files are present ===
set "MISSING="
for %%F in (
  "%RELDIR%\VUltraManager.exe"
  "%RELDIR%\README.txt"
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
