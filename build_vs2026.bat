@echo off
setlocal

where cmake >nul 2>nul || (
  echo CMake was not found in PATH.
  echo Install Visual Studio 2026 with Desktop development with C++ and CMake tools.
  pause
  exit /b 1
)



set "SOURCE_DIR=%~dp0"
if "%SOURCE_DIR:~-1%"=="\" set "SOURCE_DIR=%SOURCE_DIR:~0,-1%"

rem This project's own build folder (build\ beside this script), made again from scratch each time.
set "BUILD_DIR=%SOURCE_DIR%\build"
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
if exist "%BUILD_DIR%" goto :locked

cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G "Visual Studio 18 2026" -A x64
if errorlevel 1 (
  echo.
  echo If CMake reports that "Visual Studio 18 2026" is an unknown generator,
  echo update CMake to a version that supports the VS 2026 generator.
  goto :fail
)
rem faultrep.dll and the .asi.
cmake --build "%BUILD_DIR%" --config Release --target faultrep rottr_framework
if errorlevel 1 goto :fail



if not exist "%~dp0package\faultrep.dll" goto :missing_output
if not exist "%~dp0package\ROTTRFramework.asi" goto :missing_output

echo.
echo Built successfully:
echo   package\faultrep.dll
echo   package\ROTTRFramework.asi
echo   package\ROTTRFramework\ (scripts\ copied)
echo.
echo Copy package\ROTTRFramework.asi, package\faultrep.dll, and package\ROTTRFramework\ to the game root.
echo.
pause
exit /b 0

:locked
echo.
echo The old build folder could not be removed: a program still has files open in it.
echo Close Visual Studio, or anything else using it, and run this again.
goto :fail

:missing_output
echo.
echo Build completed but the expected package outputs were not found.
echo Build folder:
echo   %BUILD_DIR%
goto :fail

:fail
echo.
echo Build failed.
echo Build folder:
echo   %BUILD_DIR%
pause
exit /b 1
