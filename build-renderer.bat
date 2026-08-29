@echo off
REM Build the WebGPU renderer slice. Needs build-dawn.bat to have run first.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"

set VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community
set CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

"%CMAKE%" -S renderer -B build-renderer -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDawn_DIR="%~dp0vendor\dawn-install\lib\cmake\Dawn" ^
  -DSDL3_DIR="%~dp0vendor\SDL3-3.4.14\cmake"
echo --- CONFIGURE EXIT: %ERRORLEVEL% ---
if not "%ERRORLEVEL%"=="0" exit /b %ERRORLEVEL%

"%CMAKE%" --build build-renderer
echo --- BUILD EXIT: %ERRORLEVEL% ---
