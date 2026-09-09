<#
.SYNOPSIS
  Installs the repo's UE4SS mods (ue4ss-mods\) into the game's UE4SS Mods folder and
  enables them in mods.txt.

.DESCRIPTION
  Source of truth for the mods lives in this repo; the game folder is just a deploy
  target. Re-run after editing a mod.

  Also fixes two things in mods.txt, both verified by bisecting the stock mod set
  against this game:

    BPModLoaderMod    -> 0   It crashes A2 on startup: ACCESS_VIOLATION reading 0x8
                             inside UE4SS, from the engine tick, a moment after mods
                             start. With it off the game is stable for minutes; with
                             it on it dies every launch. Turning it off means BP
                             (LogicMods) mods do not load - nothing else is affected.
    ConsoleEnablerMod -> 0   Superseded by A2ConsoleUnlock, and running both would
                             have two mods fighting over the same viewport console.
                             It does NOT crash the game.

  Pass -KeepStockConsoleEnabler to leave ConsoleEnablerMod enabled, or
  -KeepBPModLoader if you want to re-test it yourself.

.EXAMPLE
  .\Install-Mods.ps1
  .\Install-Mods.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string] $GameDir = (Join-Path $PSScriptRoot '..\..\AnotherAxiom-A2-Rift\A2\Binaries\Win64'),
    [string] $ModSource = (Join-Path $PSScriptRoot '..\ue4ss-mods'),
    [switch] $KeepStockConsoleEnabler,
    [switch] $KeepBPModLoader,
    [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'
function Info($m) { Write-Host "[mods] $m" -ForegroundColor Cyan }

$gameDir = (Resolve-Path -LiteralPath $GameDir).Path

# UE4SS 3.0.1 kept everything beside the exe; newer builds moved it into ue4ss\.
# Support both so this works whichever version is installed.
$modsDir = $null
foreach ($candidate in @((Join-Path $gameDir 'ue4ss\Mods'), (Join-Path $gameDir 'Mods'))) {
    if (Test-Path -LiteralPath $candidate) { $modsDir = $candidate; break }
}
if (-not $modsDir) { throw "No UE4SS Mods folder under $gameDir (looked in ue4ss\Mods and Mods). Is UE4SS installed?" }
Info "UE4SS mods folder: $modsDir"

$modsTxt = Join-Path $modsDir 'mods.txt'
$names = @(Get-ChildItem -LiteralPath $ModSource -Directory | Select-Object -Expand Name)
if ($names.Count -eq 0) { throw "No mods found in $ModSource" }

# --- copy (or remove) the mod folders --------------------------------------------------
foreach ($name in $names) {
    $dst = Join-Path $modsDir $name
    if ($Uninstall) {
        if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force; Info "removed $name" }
    } else {
        if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force }
        Copy-Item -LiteralPath (Join-Path $ModSource $name) -Destination $dst -Recurse -Force
        Info "installed $name"
    }
}

# --- keep mods.txt in step -------------------------------------------------------------
# Format is "ModName : 1". The trailing "Keybinds : 1" must stay last, so new entries are
# inserted above the first comment line rather than appended.
$lines = @(Get-Content -LiteralPath $modsTxt)

function Set-ModEnabled([string[]] $content, [string] $mod, [int] $state) {
    $found = $false
    $out = foreach ($line in $content) {
        if ($line -match "^\s*$([regex]::Escape($mod))\s*:") { $found = $true; "$mod : $state" } else { $line }
    }
    if (-not $found -and $state -eq 1) {
        # insert before the first comment/blank block that precedes the Keybinds entry
        $idx = [Array]::FindIndex([string[]]$out, [Predicate[string]] { param($l) $l -match '^\s*;' })
        if ($idx -lt 0) { $idx = $out.Count }
        $out = @($out[0..([Math]::Max($idx - 1, 0))]) + @("$mod : $state") + @($out[$idx..($out.Count - 1)])
    }
    return $out
}

foreach ($name in $names) {
    $lines = Set-ModEnabled $lines $name ($(if ($Uninstall) { 0 } else { 1 }))
}
if (-not $KeepStockConsoleEnabler) {
    $lines = Set-ModEnabled $lines 'ConsoleEnablerMod' ($(if ($Uninstall) { 1 } else { 0 }))
    Info $(if ($Uninstall) { "re-enabled stock ConsoleEnablerMod" } else { "disabled stock ConsoleEnablerMod (A2ConsoleUnlock replaces it)" })
}
if (-not $KeepBPModLoader -and -not $Uninstall) {
    $lines = Set-ModEnabled $lines 'BPModLoaderMod' 0
    Info "disabled BPModLoaderMod (crashes A2 on startup - see the script header)"
}

Set-Content -LiteralPath $modsTxt -Value $lines -Encoding UTF8
Info "updated $modsTxt"
Get-Content -LiteralPath $modsTxt | Where-Object { $_ -match ':' -and $_ -notmatch '^\s*;' } | ForEach-Object { "        $_" }
