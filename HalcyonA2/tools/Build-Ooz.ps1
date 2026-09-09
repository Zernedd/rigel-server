<#
.SYNOPSIS
  Builds tools\oodle\ooz.dll - the Oodle (Kraken) DECOMPRESSOR used by a2urlpatch.py.

.DESCRIPTION
  UE paks compress their entries with Oodle, so reading a packaged config means
  decompressing Kraken. Oodle itself is proprietary and not redistributable, so this
  builds the open-source clean-room decompressor `ooz` (https://github.com/powzix/ooz)
  and wraps it in a tiny DLL export that Python can call through ctypes.

  Decompression only. There is no free Kraken *encoder*, which is why a2urlpatch
  re-encodes patched entries with Zlib instead (UE supports it natively for paks).

  Requires: git, and Visual Studio 2022 with the C++ toolset.

.EXAMPLE
  .\Build-Ooz.ps1
#>
[CmdletBinding()]
param(
    [string] $WorkDir = (Join-Path $env:TEMP 'ooz-build'),
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
function Info($m) { Write-Host "[ooz] $m" -ForegroundColor Cyan }

$outDir = Join-Path $PSScriptRoot 'oodle'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

if ($Clean -and (Test-Path $WorkDir)) { Remove-Item $WorkDir -Recurse -Force }
if (-not (Test-Path (Join-Path $WorkDir 'kraken.cpp'))) {
    Info "cloning ooz into $WorkDir"
    git clone --depth 1 https://github.com/powzix/ooz.git $WorkDir
} else {
    Info "reusing $WorkDir"
}

# The ctypes entry point. Kept here (not in the clone) so a re-clone cannot lose it.
$wrapper = Join-Path $PSScriptRoot 'oodle\oozdll.cpp'
if (-not (Test-Path $wrapper)) { throw "missing $wrapper" }
Copy-Item $wrapper $WorkDir -Force

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ toolset was not found." }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

Info "compiling (x64, /O2)"
# /FIsys/stat.h: ooz's CLI main() uses struct stat without including it, which MSVC rejects.
$cmd = "`"$vcvars`" >nul && cd /d `"$WorkDir`" && cl /nologo /LD /O2 /EHsc /D_CRT_SECURE_NO_WARNINGS " +
       "/FIsys/stat.h /FIsys/types.h kraken.cpp bitknit.cpp lzna.cpp oozdll.cpp /Fe:ooz.dll"
cmd /c $cmd | Select-String -Pattern 'error|warning C4700' | ForEach-Object { Write-Host "  $_" }
if (-not (Test-Path (Join-Path $WorkDir 'ooz.dll'))) { throw "build produced no ooz.dll" }

Copy-Item (Join-Path $WorkDir 'ooz.dll') $outDir -Force
Info "-> $(Join-Path $outDir 'ooz.dll') ($([math]::Round((Get-Item (Join-Path $outDir 'ooz.dll')).Length/1KB)) KB)"
Info "verify with:  python a2urlpatch.py find ..\..\main.33694970.com.AnotherAxiom.A2.obb aa-mothership.com"
