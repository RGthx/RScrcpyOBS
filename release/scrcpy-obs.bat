@echo off
rem ===========================================================================
rem  RscrcpyOBS - start scrcpy so OBS can pick up the picture and the sound.
rem
rem  This file must sit next to scrcpy.exe, inside the release package:
rem      RscrcpyOBS-win64-<version>\scrcpy\scrcpy-obs.bat
rem
rem  Usage: point a shortcut at this .bat and put your options in the
rem  shortcut's Target:
rem      D:\RscrcpyOBS-win64\scrcpy\scrcpy-obs.bat --obs-video ^
rem        --obs-video-name k60 --obs-audio --obs-audio-name k60 ^
rem        --audio-source=playback --no-audio-playback --no-window ^
rem        --audio-codec=raw -b 64M -n
rem  Then in OBS add a "scrcpy Capture (rscrcpy_capture)" source whose video
rem  shared memory name is the same (k60). One shortcut per device or preset,
rem  exactly like plain scrcpy.
rem
rem  Nothing here is RscrcpyOBS-specific except the --obs-* options: every other
rem  option is plain scrcpy and behaves exactly as upstream.
rem ===========================================================================
setlocal

if not exist "%~dp0scrcpy.exe" (
  echo.
  echo [RscrcpyOBS] scrcpy.exe is not next to this script:
  echo     %~dp0
  echo.
  echo   This wrapper only works inside the release package, for example:
  echo     RscrcpyOBS-win64-^<version^>\scrcpy\scrcpy-obs.bat
  echo.
  pause
  exit /b 2
)

rem ---------------------------------------------------------------------------
rem Give the child a private temp directory: adb writes a log there every time
rem its daemon starts, and this keeps that file inside the package.
rem ---------------------------------------------------------------------------
set "RSCRCPY_TMP=%~dp0tmp"
if not exist "%RSCRCPY_TMP%" mkdir "%RSCRCPY_TMP%" 2>nul
if exist "%RSCRCPY_TMP%" (
  set "TMP=%RSCRCPY_TMP%"
  set "TEMP=%RSCRCPY_TMP%"
)

rem Tell scrcpy where its server is, instead of relying on a path baked in at
rem build time.
set "SCRCPY_SERVER_PATH=%~dp0scrcpy-server"
"%~dp0scrcpy.exe" %*
endlocal
