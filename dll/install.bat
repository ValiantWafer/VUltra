@echo off
setlocal
set GAME=J:\SteamLibrary\steamapps\common\Vagante
set SRC=%~dp0openal32.dll

if not exist "%GAME%\openal32.dll" (echo ERROR: game openal32.dll not found at "%GAME%" & exit /b 1)
if not exist "%SRC%" (echo ERROR: built proxy not found at "%SRC%" & exit /b 1)

rem Pristine one-time backup of the original Steam dll
if not exist "%GAME%\openal32.orig.dll" (
    copy /y "%GAME%\openal32.dll" "%GAME%\openal32.orig.dll" >nul
    echo Backed up original -> openal32.orig.dll
)

rem Forward target = the real OpenAL (from the pristine backup, so re-running is safe)
copy /y "%GAME%\openal32.orig.dll" "%GAME%\openal32_real.dll" >nul
echo Installed real OpenAL -> openal32_real.dll

rem Our proxy takes the openal32.dll slot
copy /y "%SRC%" "%GAME%\openal32.dll" >nul
echo Installed proxy        -> openal32.dll

rem Drop a default config to edit (kept if you already have one)
if not exist "%GAME%\vultramod.ini" (
    copy /y "%~dp0vultramod.ini" "%GAME%\vultramod.ini" >nul
    echo Installed config       -> vultramod.ini
) else (
    echo Kept existing config   -> vultramod.ini
)

echo.
echo DONE.  Edit "%GAME%\vultramod.ini" to choose spells (1=on, 0=off), then launch.
endlocal
