@echo off
rem ## Usage example for finding the install path of Unreal 4.24
rem ## ./GetUnrealInstallPath.bat 4.24

set UnrealInstallPath=
for /F "tokens=2,*" %%A IN ('reg query "HKLM\SOFTWARE\EpicGames\Unreal Engine\%1" /reg:64 /v "InstalledDirectory"') do (
    IF exist "%%B" (
        set "UnrealInstallPath=%%B"
    )
)
IF "%UnrealInstallPath%" == "" goto :UnrealInstallPathNotFound

:Success
echo UnrealInstallPath set to %UnrealInstallPath%
exit /B 0

:UnrealInstallPathNotFound
echo Install path for Unreal Engine %1 not found!
set UnrealInstallPath=%ProgramFiles%\Epic Games\UE_%1
exit /B 2
