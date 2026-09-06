@echo off
setlocal

:: ============================================
:: RAVEN CS2 INJECTOR - BUILD SCRIPT
:: ============================================

echo.
echo =========================================
echo   RAVEN CS2 INJECTOR v2.0.0
echo   Build Script
echo =========================================
echo.

:: Check for Visual Studio 2022
if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
    echo [+] Visual Studio 2022 Community found
    goto :build
)

if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe"
    echo [+] Visual Studio 2022 Professional found
    goto :build
)

if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
    echo [+] Visual Studio 2022 Enterprise found
    goto :build
)

:: Check for Visual Studio 2019
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe"
    echo [+] Visual Studio 2019 Community found
    goto :build
)

if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\MSBuild\Current\Bin\MSBuild.exe"
    echo [+] Visual Studio 2019 Professional found
    goto :build
)

:: MSBuild not found
echo [-] ERROR: Visual Studio 2019/2022 not found!
echo.
echo Please install Visual Studio with C++ development tools:
echo https://visualstudio.microsoft.com/downloads/
echo.
echo Or open Injector.sln manually and build with IDE.
echo.
pause
exit /b 1

:build
echo.
echo [*] Building Injector project...
echo     Configuration: Release
echo     Platform: x64
echo.

cd /d "%~dp0Injector"

"%MSBUILD%" Injector.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo

if errorlevel 1 (
    echo.
    echo [-] BUILD FAILED!
    echo.
    echo Check error messages above.
    echo You may need to:
    echo   1. Install Windows 10 SDK
    echo   2. Install C++ Build Tools
    echo   3. Check BlackBone library in lib\ folder
    echo.
    pause
    exit /b 1
)

echo.
echo =========================================
echo   BUILD SUCCESSFUL!
echo =========================================
echo.
echo Output: x64\Release\raven.exe
echo.

:: Check if file was created
if exist "x64\Release\raven.exe" (
    echo [*] Copying to TEK_PAKET folder...
    
    if not exist "..\..\TEK_PAKET" (
        echo [-] TEK_PAKET folder not found, creating...
        mkdir "..\..\TEK_PAKET"
    )
    
    copy /Y "x64\Release\raven.exe" "..\..\TEK_PAKET\RavenInjector_v2.exe" >nul
    
    if exist "..\..\TEK_PAKET\RavenInjector_v2.exe" (
        echo [+] Copied to: TEK_PAKET\RavenInjector_v2.exe
        echo.
        
        :: Get file size
        for %%A in ("..\..\TEK_PAKET\RavenInjector_v2.exe") do (
            set size=%%~zA
        )
        
        echo File Size: %size% bytes
        echo.
        
        echo [SUCCESS] Build complete and ready to use!
    ) else (
        echo [-] Failed to copy file
    )
) else (
    echo [-] Output file not found!
)

echo.
echo =========================================
echo.
echo To test the injector:
echo   1. Run as Administrator
echo   2. Start CS2
echo   3. The injector will auto-detect and inject
echo.
echo Logs will be created in: logs\injector_*.log
echo.
pause
