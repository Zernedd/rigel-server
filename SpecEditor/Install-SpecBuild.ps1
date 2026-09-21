<#
.SYNOPSIS
  Create (or refresh) the Spec Editor build: a copy of a stock A2 client with the editor mod installed.

.DESCRIPTION
  Makes a separate build folder so the editor never touches a build you play on. The mod ships as
  dsound.dll next to the exe - A2-Win64-Shipping.exe statically imports dsound by ordinal, so the
  loader picks ours up first and every ordinal is forwarded to the real system dsound.

  The game content is large, so by default the copy is made with hard links (-Hardlink), which costs
  almost no disk and takes seconds instead of minutes. Pass -FullCopy for an independent copy.

.PARAMETER Source
  A stock client build to base this on. Defaults to specnovbuild.

.PARAMETER Dest
  Where to put the editor build. Defaults to SpecEditorBuild beside the other builds.

.EXAMPLE
  .\Install-SpecBuild.ps1
  .\Install-SpecBuild.ps1 -ModOnly        # just refresh dsound.dll after a rebuild
#>
[CmdletBinding()]
param(
    [string] $Source,
    [string] $Dest,
    [switch] $FullCopy,
    [switch] $ModOnly
)

$ErrorActionPreference = 'Stop'

# Defaults are resolved here, not in param(): Windows PowerShell 5.1 leaves $PSScriptRoot empty inside
# param() defaults when the script is started with -File, which turned the destination into
# "\..\SpecEditorBuild" and failed with "destination has no Binaries\Win64".
$scriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $Source) { $Source = Join-Path $scriptDir '..\specnovbuild' }
if (-not $Dest)   { $Dest   = Join-Path $scriptDir '..\SpecEditorBuild' }
function Info($m) { Write-Host "[spec-build] $m" -ForegroundColor Cyan }
function Good($m) { Write-Host "[spec-build] $m" -ForegroundColor Green }

$mod = Join-Path $scriptDir 'mod\dsound.dll'
if (-not (Test-Path $mod)) { throw "mod not built yet - run mod\build.ps1 first" }

$destBin = Join-Path $Dest 'A2\Binaries\Win64'

if (-not $ModOnly) {
    if (-not (Test-Path $Source)) { throw "source build not found: $Source" }
    $srcExe = Join-Path $Source 'A2\Binaries\Win64\A2-Win64-Shipping.exe'
    if (-not (Test-Path $srcExe)) { throw "not a client build (no A2-Win64-Shipping.exe): $Source" }

    if (Test-Path $Dest) {
        Info "destination exists, leaving content in place: $Dest"
    } else {
        Info "creating $Dest from $Source"
        if ($FullCopy) {
            Copy-Item $Source $Dest -Recurse -Force
        } else {
            # Hard-link the tree: same bytes on disk, independent paths. Directories are created for
            # real; only files are linked. The OBB/pak files are the bulk and never change.
            New-Item -ItemType Directory -Force -Path $Dest | Out-Null
            Push-Location $Source
            try {
                Get-ChildItem -Recurse -Directory | ForEach-Object {
                    $rel = $_.FullName.Substring((Get-Location).Path.Length + 1)
                    New-Item -ItemType Directory -Force -Path (Join-Path $Dest $rel) | Out-Null
                }
                $n = 0
                Get-ChildItem -Recurse -File | ForEach-Object {
                    $rel = $_.FullName.Substring((Get-Location).Path.Length + 1)
                    $target = Join-Path $Dest $rel
                    if (-not (Test-Path $target)) {
                        cmd /c mklink /H "`"$target`"" "`"$($_.FullName)`"" > $null 2>&1
                        if ($LASTEXITCODE -ne 0) { Copy-Item $_.FullName $target -Force }
                        $n++
                    }
                }
                Info "linked $n file(s)"
            } finally { Pop-Location }
        }
    }
}

if (-not (Test-Path $destBin)) { throw "destination has no Binaries\Win64: $destBin" }

# The Oculus platform DLLs are NOT in every build (specnovbuild ships without them) and the client dies
# on start-up without them - it looks exactly like "the mod broke the game", which cost a debugging
# round here. Take them from any build that has them.
foreach ($dll in 'LibOVRP2P64_1.dll', 'LibOVRPlatformImpl64_1.dll') {
    if (Test-Path (Join-Path $destBin $dll)) { continue }
    $donor = Get-ChildItem (Join-Path $scriptDir '..') -Directory |
             ForEach-Object { Join-Path $_.FullName "A2\Binaries\Win64\$dll" } |
             Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($donor) { Copy-Item $donor (Join-Path $destBin $dll) -Force; Info "added missing $dll" }
    else { Write-Warning "$dll not found in any build - the client may exit on start-up" }
}

# Copy a file into the editor build without ever writing through a hard link. This build is hard-linked
# to the stock one, and Copy-Item onto an existing link overwrites the SHARED data -- it would silently
# change the stock build too. Removing the target first breaks the link and gives this build its own copy.
# (Always Copy, never Move-Item: a Move onto an existing file fails silently here and leaves the old
# payload live, which has cost a full launch cycle before.)
function Put($from, $to) {
    if (Test-Path -LiteralPath $to) { Remove-Item -LiteralPath $to -Force }
    Copy-Item -LiteralPath $from -Destination $to
}

Put $mod (Join-Path $destBin 'dsound.dll')
$sz = (Get-Item (Join-Path $destBin 'dsound.dll')).Length
Good "editor mod installed -> $destBin\dsound.dll ($sz bytes)"

# The full LE prefab catalogue. Without it the palette can only list classes already loaded in memory,
# which on the station is 2 of ~97. Regenerate with tools\make_catalogue.py.
$cat = Join-Path $scriptDir 'mod\le_catalogue.txt'
if (Test-Path $cat) {
    Put $cat (Join-Path $destBin 'le_catalogue.txt')
    $n = @(Get-Content $cat | Where-Object { $_ -and -not $_.StartsWith('#') }).Count
    Good "catalogue installed -> $destBin\le_catalogue.txt ($n LE prefabs)"
} else { Write-Warning "mod\le_catalogue.txt missing - run tools\make_catalogue.py; palette will be limited" }

$exe = Join-Path $destBin 'A2-Win64-Shipping.exe'
Good "launch:  `"$exe`" -windowed -ResX=1600 -ResY=900"
Good "INSERT toggles the editor UI.  Log: %TEMP%\spec_editor.log"
