<#
.SYNOPSIS
  Builds HalcyonA2.sln with the installed VS 2022 MSBuild (no VS IDE required).
.EXAMPLE
  .\Build.ps1                     # Release|x64
  .\Build.ps1 -Configuration Debug
  .\Build.ps1 -Rebuild
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release',
    # Which A2 build to target; picks gamesdk\<build>\ and the GameOffsets.h block.
    [ValidateSet('20996', '22284')] [string] $GameBuild = '22284',
    [switch] $Rebuild
)

$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw "vswhere.exe not found - is Visual Studio installed?" }

$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No VS install with the C++ toolset (Desktop development with C++) was found." }

$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) { throw "MSBuild.exe not found under $vs." }

$sln = (Resolve-Path (Join-Path $PSScriptRoot '..\HalcyonA2.sln')).Path
$sdk = Join-Path $PSScriptRoot "..\HalcyonA2\gamesdk\$GameBuild\SDK.hpp"
if (-not (Test-Path -LiteralPath $sdk)) {
    throw "No SDK for build $GameBuild (expected $sdk). Generate it: .\Dump-A2.ps1 -GameExe <that build's exe>"
}

$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
Write-Host "[build] $target $Configuration|x64 for game build $GameBuild with $msbuild" -ForegroundColor Cyan
& $msbuild $sln /t:$target /p:Configuration=$Configuration /p:Platform=x64 /p:GameBuild=$GameBuild /m /nologo /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }

$dll = Join-Path $PSScriptRoot "..\build\$GameBuild\x64\$Configuration\HalcyonA2.dll"
Write-Host "[build] -> $((Resolve-Path $dll).Path)" -ForegroundColor Green
