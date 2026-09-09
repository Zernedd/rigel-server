<#
.SYNOPSIS
  Builds the two windowed test clients used for replication testing:

    ..\..\PlayerNovBuild   the PLAYER  - console unlock + entitlement patch + A2PlayerControl
                                         (F1 = exit spectator, WASD/Space/Ctrl = fly, Shift = boost)
    ..\..\specnovbuild     the SPECTATOR - console unlock + entitlement patch only

.DESCRIPTION
  Each folder is a full copy of the Nov15 build with UE4SS dropped in beside the exe, so the two
  can run side by side as real windowed clients (no -nullrhi) and actually show replication.

  Both get A2EntitlementPatch, not just the console mod: this machine has no Meta entitlement for
  app 23916854551246326, and without the patch a client quits itself ~30 s after launch
  ("Could not verify entitlement status ... A Shipping build would exit at this point"). The mod
  now locates the check by signature, so it works on 22284 as well as 20996.

  Re-run any time - it refreshes UE4SS and the mods without recopying the game unless -Force.

.EXAMPLE
  .\New-TestClients.ps1
  .\New-TestClients.ps1 -Force        # recopy the game files too
  .\New-TestClients.ps1 -ModsOnly     # just refresh ue4ss\Mods in both folders
#>
[CmdletBinding()]
param(
    [string] $SourceBuild = (Join-Path $PSScriptRoot '..\..\Nov15'),
    [string] $Ue4ssSource  = (Join-Path $PSScriptRoot '..\..\AnotherAxiom-A2-Rift\A2\Binaries\Win64\ue4ss'),
    [string] $ModSource    = (Join-Path $PSScriptRoot '..\ue4ss-mods'),
    [string] $Root         = (Join-Path $PSScriptRoot '..\..'),
    [switch] $Force,
    [switch] $ModsOnly
)

$ErrorActionPreference = 'Stop'
function Info($m) { Write-Host "[clients] $m" -ForegroundColor Cyan }
function Good($m) { Write-Host "[clients] $m" -ForegroundColor Green }

$SourceBuild = (Resolve-Path -LiteralPath $SourceBuild).Path
$Ue4ssSource = (Resolve-Path -LiteralPath $Ue4ssSource).Path
$ModSource   = (Resolve-Path -LiteralPath $ModSource).Path
$Root        = (Resolve-Path -LiteralPath $Root).Path

# name -> the mods it should run
$targets = @(
    @{ Name = 'PlayerNovBuild'; Mods = @('A2ConsoleUnlock', 'A2EntitlementPatch', 'A2PlayerControl') }
    @{ Name = 'specnovbuild';   Mods = @('A2ConsoleUnlock', 'A2EntitlementPatch') }
)

# UE4SS's own mods. BPModLoaderMod crashes A2 on startup and ConsoleEnablerMod fights
# A2ConsoleUnlock over the same viewport console - both verified by bisection, both off.
$stockMods = [ordered]@{
    'CheatManagerEnablerMod' = 1
    'ConsoleCommandsMod'     = 1
    'ConsoleEnablerMod'      = 0
    'SplitScreenMod'         = 0
    'LineTraceMod'           = 0
    'BPML_GenericFunctions'  = 1
    'BPModLoaderMod'         = 0
}

foreach ($t in $targets) {
    $dst    = Join-Path $Root $t.Name
    $binDst = Join-Path $dst 'A2\Binaries\Win64'

    # --- game files -------------------------------------------------------------------
    if ($ModsOnly) {
        if (-not (Test-Path -LiteralPath $binDst)) { throw "$($t.Name) does not exist yet; run without -ModsOnly first." }
        Info "$($t.Name): mods only"
    }
    else {
        if ((Test-Path -LiteralPath $binDst) -and -not $Force) {
            Info "$($t.Name): game files already present (use -Force to recopy)"
        }
        else {
            Info "$($t.Name): copying the Nov15 build (~3.2 GB, takes a minute)..."
            $null = robocopy $SourceBuild $dst /E /NFL /NDL /NJH /NJS /NP /MT:16
            if ($LASTEXITCODE -ge 8) { throw "robocopy failed for $($t.Name) (exit $LASTEXITCODE)" }
        }
    }

    # --- UE4SS ------------------------------------------------------------------------
    $ue4ssDst = Join-Path $binDst 'ue4ss'
    Info "$($t.Name): installing UE4SS"
    $null = robocopy $Ue4ssSource $ue4ssDst /E /NFL /NDL /NJH /NJS /NP /XF UE4SS.log
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed for UE4SS into $($t.Name)" }

    # dwmapi.dll beside the exe is the proxy that loads UE4SS.
    $proxySrc = Join-Path (Split-Path $Ue4ssSource -Parent) 'dwmapi.dll'
    if (-not (Test-Path -LiteralPath $proxySrc)) { throw "UE4SS proxy dwmapi.dll not found next to $Ue4ssSource" }
    Copy-Item -LiteralPath $proxySrc -Destination (Join-Path $binDst 'dwmapi.dll') -Force

    # --- our mods ---------------------------------------------------------------------
    $modsDst = Join-Path $ue4ssDst 'Mods'
    New-Item -ItemType Directory -Force -Path $modsDst | Out-Null

    # Remove any of ours that this target should NOT have, so specnovbuild really is
    # console-only and cannot accidentally inherit A2PlayerControl.
    foreach ($ours in (Get-ChildItem -LiteralPath $ModSource -Directory | Select-Object -Expand Name)) {
        $p = Join-Path $modsDst $ours
        if ($ours -notin $t.Mods -and (Test-Path -LiteralPath $p)) {
            Remove-Item -LiteralPath $p -Recurse -Force
            Info "$($t.Name): removed $ours"
        }
    }
    foreach ($m in $t.Mods) {
        $src = Join-Path $ModSource $m
        if (-not (Test-Path -LiteralPath $src)) { throw "mod source missing: $src" }
        $null = robocopy $src (Join-Path $modsDst $m) /E /NFL /NDL /NJH /NJS /NP
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed for mod $m" }
        Info "$($t.Name): installed $m"
    }

    # --- mods.txt ---------------------------------------------------------------------
    $lines = @()
    foreach ($k in $stockMods.Keys) { $lines += ('{0} : {1}' -f $k, $stockMods[$k]) }
    foreach ($m in $t.Mods)         { $lines += ('{0} : 1' -f $m) }
    $lines += ''
    $lines += '; Built-in keybinds, do not move up!'
    $lines += 'Keybinds : 1'
    Set-Content -LiteralPath (Join-Path $modsDst 'mods.txt') -Value $lines -Encoding ASCII

    Good "$($t.Name) ready -> $binDst\A2-Win64-Shipping.exe"
}

Write-Host ''
Good 'Both clients built. Start them with the batch files in this folder:'
Write-Host '    Start-Server-NoAuth.bat     the server (auth gate off)'
Write-Host '    Start-Player.bat            the PLAYER client  (F1 = exit spectator, WASD = fly)'
Write-Host '    Start-Spectator.bat         the SPECTATOR client (console only)'

$global:LASTEXITCODE = 0
exit 0
