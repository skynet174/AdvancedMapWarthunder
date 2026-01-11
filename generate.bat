@echo off
echo ========================================
echo    Generating Visual Studio Solution
echo ========================================
echo.

:: Создание необходимых директорий
:: Creating the necessary directories
if not exist "Build" mkdir "Build"
if not exist "Bin" mkdir "Bin"
if not exist "Source" mkdir "Source"

:: Запуск Premake5 для генерации VS2026 solution
:: Running Premake5 to generate a VS2026 solution
echo Running Premake5...
premake5 vs2022

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] Failed to generate solution!
    pause
    exit /b 1
)

echo.
echo ========================================
echo    Solution generated successfully!
echo    Location: WarThunderAdvanced.sln
echo ========================================
echo.
pause

