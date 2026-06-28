@echo off
setlocal
set GAME=J:\SteamLibrary\steamapps\common\Vagante
if not exist "%GAME%\openal32.orig.dll" (echo Nothing to restore (no backup found). & exit /b 1)
copy /y "%GAME%\openal32.orig.dll" "%GAME%\openal32.dll" >nul
del /q "%GAME%\openal32_real.dll" 2>nul
del /q "%GAME%\vultra.log" "%GAME%\noflamepillar.log" 2>nul
echo Restored original openal32.dll and removed the mod.
endlocal
