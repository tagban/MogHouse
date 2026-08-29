@echo off
REM Configure and build the native engine WITHOUT the C++ standard library
REM module. See vendor/vulkan-module/README.md for why that matters.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"

set VSROOT=C:\Program Files\Microsoft Visual Studio\18\Community
set CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
set VULKAN_SDK=C:\VulkanSDK\1.4.357.0

"%CMAKE%" -S ffxi-engine -B build-engine -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DSDL3_DIR="%~dp0vendor\SDL3-3.4.14\cmake" ^
  -DVulkanMemoryAllocator_DIR="%~dp0vendor\vma-install\share\cmake\VulkanMemoryAllocator" ^
  -DSLANGC="%VULKAN_SDK%\Bin\slangc.exe" ^
  -DGLM_ROOT_DIR="%~dp0vendor\glm" ^
  -DPORTJEUNO_VULKAN_CPPM="%~dp0vendor\vulkan-module\vulkan.cppm"
echo --- CONFIGURE EXIT: %ERRORLEVEL% ---
if not "%ERRORLEVEL%"=="0" exit /b %ERRORLEVEL%

"%CMAKE%" --build build-engine
echo --- BUILD EXIT: %ERRORLEVEL% ---
