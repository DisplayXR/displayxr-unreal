@echo off
rem ## Package DisplayXR.uplugin for a specific Unreal Engine version.
rem ## Usage: PackagePlugin.bat 5.6

setlocal

if "%~1"=="" (
    echo Usage: PackagePlugin.bat ^<UE_VERSION^>
    echo Example: PackagePlugin.bat 5.6
    exit /b 1
)

rem ## Resolve Unreal install path via GetUnrealInstallPath.bat
call "%~dp0GetUnrealInstallPath.bat" %1
if not exist "%UnrealInstallPath%" (
    echo Unreal Engine %1 not found at: %UnrealInstallPath%
    exit /b 2
)

set RunUATFile="%UnrealInstallPath%\Engine\Build\BatchFiles\RunUAT.bat"
set PluginFile=%~dp0..\DisplayXR.uplugin
set PackageDirectory=%~dp0..\Packages\DisplayXR_%1

echo Packaging DisplayXR plugin for UE%1...
echo   Plugin:  %PluginFile%
echo   Output:  %PackageDirectory%

call %RunUATFile% BuildPlugin ^
    -Plugin="%PluginFile%" ^
    -Package="%PackageDirectory%" ^
    -CreateSubFolder ^
    -TreatWarningsAsErrors=false
if %errorlevel% neq 0 exit /b %errorlevel%

echo Package succeeded: %PackageDirectory%
exit /b 0
