# Builds the plugin's static FFmpeg on Windows: loads the Visual Studio x64
# developer environment and runs build.sh in MSYS2 with that environment.
#
# Usage: build-windows.ps1 [-Prefix <dir>] [-Msys2 <dir>]

param(
    [string] $Prefix = "$PSScriptRoot/../../.deps-ffmpeg/windows-x64",
    [string] $Msys2 = 'C:/msys64'
)

$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ( -not $vs ) { throw 'Visual Studio with the C++ x64 tools was not found' }

# import the variables vcvars64.bat sets into this process
$env:PATH = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer;$env:PATH"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object {
    if ( $_ -match '^([^=]+)=(.*)$' ) { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

New-Item -ItemType Directory -Force $Prefix | Out-Null
$prefixUnix = (Resolve-Path $Prefix).Path -replace '\\', '/' -replace '^([A-Za-z]):', '/$1'
$script = (Resolve-Path "$PSScriptRoot/build.sh").Path -replace '\\', '/' -replace '^([A-Za-z]):', '/$1'

$env:MSYS2_PATH_TYPE = 'inherit'
$env:CHERE_INVOKING = '1'
& "$Msys2/usr/bin/bash.exe" -lc "bash '$script' '$prefixUnix'"
if ( $LASTEXITCODE -ne 0 ) { throw "FFmpeg build failed ($LASTEXITCODE)" }
