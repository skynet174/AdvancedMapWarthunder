@echo off
echo ========================================
echo    Generating Visual Studio Solution
echo ========================================
echo.

:: Проверяем наличие premake5.exe
:: Check if premake5.exe exists
if not exist "premake5.exe" (
    echo premake5.exe not found. Downloading...
    echo.
    
    :: Скачиваем premake5 с GitHub
    :: Download premake5 from GitHub
    echo Downloading premake-5.0.0-beta7-windows.zip...
    
    :: Используем PowerShell для скачивания и распаковки
    :: Use PowerShell to download and extract
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "$url = 'https://github.com/premake/premake-core/releases/download/v5.0.0-beta7/premake-5.0.0-beta7-windows.zip'; " ^
        "$zipName = 'premake-5.0.0-beta7-windows.zip'; " ^
        "$currentDir = $PWD.Path; " ^
        "$zipPath = [System.IO.Path]::Combine($currentDir, $zipName); " ^
        "$premakePath = [System.IO.Path]::Combine($currentDir, 'premake5.exe'); " ^
        "try { " ^
        "    $ProgressPreference = 'SilentlyContinue'; " ^
        "    Write-Host 'Current directory:' $currentDir; " ^
        "    Write-Host 'Downloading from:' $url; " ^
        "    Write-Host 'Saving to:' $zipPath; " ^
        "    Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing; " ^
        "    Write-Host 'Download completed. File size:' (Get-Item $zipPath).Length 'bytes'; " ^
        "    if (-not (Test-Path $zipPath)) { " ^
        "        Write-Host 'Error: Downloaded file not found!'; " ^
        "        exit 1 " ^
        "    } " ^
        "    Write-Host 'Extracting archive...'; " ^
        "    Expand-Archive -Path $zipPath -DestinationPath $currentDir -Force; " ^
        "    Write-Host 'Extraction completed.'; " ^
        "    Remove-Item $zipPath -Force; " ^
        "    Write-Host 'Cleanup completed.'; " ^
        "    if (Test-Path $premakePath) { " ^
        "        Write-Host 'premake5.exe found successfully!' " ^
        "    } else { " ^
        "        Write-Host 'Warning: premake5.exe not found in current directory after extraction.'; " ^
        "        Write-Host 'Checking subdirectories...'; " ^
        "        $found = Get-ChildItem -Path $currentDir -Filter 'premake5.exe' -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1; " ^
        "        if ($found) { " ^
        "            Write-Host 'Found at:' $found.FullName; " ^
        "            Copy-Item $found.FullName -Destination $premakePath -Force; " ^
        "            Write-Host 'premake5.exe copied to current directory.' " ^
        "        } else { " ^
        "            Write-Host 'Error: premake5.exe not found in archive.'; " ^
        "            exit 1 " ^
        "        } " ^
        "    } " ^
        "} catch { " ^
        "    Write-Host 'Error:' $_.Exception.Message; " ^
        "    Write-Host 'Error details:' $_.Exception; " ^
        "    if (Test-Path $zipPath) { Remove-Item $zipPath -Force -ErrorAction SilentlyContinue } " ^
        "    exit 1 " ^
        "}"
    
    if %ERRORLEVEL% NEQ 0 (
        echo.
        echo [ERROR] Failed to download or extract premake5!
        pause
        exit /b 1
    )
    
    echo.
    echo premake5.exe downloaded and extracted successfully!
    echo.
)

:: Создание необходимых директорий
:: Creating the necessary directories
if not exist "Build" mkdir "Build"
if not exist "Bin" mkdir "Bin"
if not exist "Source" mkdir "Source"

:: Запуск Premake5 для генерации VS2026 solution
:: Running Premake5 to generate a VS2026 solution
echo Running Premake5...

:: Проверяем, что premake5.exe существует перед запуском
:: Check that premake5.exe exists before running
if not exist "premake5.exe" (
    echo.
    echo [ERROR] premake5.exe not found after download!
    pause
    exit /b 1
)

.\premake5.exe vs2022

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

