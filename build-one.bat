@echo off
REM Build a single object without reconfiguring, for tight iteration on one
REM translation unit. Usage: build-one.bat <ninja-target>
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
set NINJA=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
"%NINJA%" -C build-engine %*
echo --- EXIT: %ERRORLEVEL% ---
