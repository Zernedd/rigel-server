<#
.SYNOPSIS
  Generates the Dumper-7 SDK + .usmap mappings for the A2 (Orion Drift) client and
  installs them into the HalcyonA2 project.

.DESCRIPTION
  Follows the documented Dumper-7 flow (README / UsingTheSDK):
    1. Write a GLOBAL Dumper-7 profile at C:\Dumper-7\Dumper-7.ini with SleepTimeout,
       so the dumper waits after injection instead of dumping the instant it loads
       (GObjects/GNames/KismetSystemLibrary must be live first). The game install is
       left untouched - no per-game ini is dropped next to the exe.
    2. Launch A2-Win64-Shipping.exe headless (-nullrhi), the same mode the server runs in.
    3. Inject Dumper-7.dll (CreateRemoteThread + LoadLibraryW).
    4. Wait for C:\Dumper-7\<GameVersion>-<GameName>\ which Dumper-7 fills with:
         CppSDK\SDK.hpp + CppSDK\SDK\   the C++ SDK the DLL compiles against
         Mappings\*.usmap               the mappings ("map") for asset tooling
         IDAMappings\                   IDA symbol scripts
         Dumpspace\                     json dumps
         ObjectArray / GNames dumps
    5. Copy the SDK into HalcyonA2\HalcyonA2\ and the mappings into tools\Mappings\.

.EXAMPLE
  .\Dump-A2.ps1
  .\Dump-A2.ps1 -AttachOnly           # game already running (e.g. launched with a headset)
  .\Dump-A2.ps1 -InitWaitSeconds 90   # slow box / cold cache
#>
[CmdletBinding()]
param(
    [string] $GameExe = (Join-Path $PSScriptRoot '..\..\AnotherAxiom-A2-Rift\A2\Binaries\Win64\A2-Win64-Shipping.exe'),
    [string] $DumperDll = (Join-Path $PSScriptRoot 'Dumper7\Dumper-7.dll'),
    [string] $DumpRoot = 'C:\Dumper-7',
    [int]    $InitWaitSeconds = 60,     # engine init before we inject
    [int]    $SleepTimeout = 20,        # Dumper-7 ini: extra delay after injection before it dumps
    [int]    $DumpTimeoutSeconds = 900,
    [switch] $AttachOnly,
    [switch] $KeepGameRunning,
    # Skip launch/inject and (re)install the SDK + mappings from a dump that already exists,
    # e.g. -UseExistingDump 'C:\Dumper-7\5.4.2-20996+++A2+Release-A2'.
    [string] $UseExistingDump,
    # Only produce the dump under C:\Dumper-7; do not touch the project's SDK or mappings.
    # Use this to dump a second game build for comparison without replacing the active SDK.
    [switch] $NoInstall,
    # Which gamesdk\<build>\ folder to install into. Defaults to the build number Dumper-7
    # reports for the dump (the "22284" in 5.4.2-22284+++A2+Release-A2).
    [string] $GameBuild
)

$ErrorActionPreference = 'Stop'
$projDir = Join-Path $PSScriptRoot '..\HalcyonA2'

function Info($m) { Write-Host "[dump] $m" -ForegroundColor Cyan }

$proc = $null
$dumpDir = $null

if ($UseExistingDump) {
    $dumpDir = (Resolve-Path -LiteralPath $UseExistingDump).Path
    Info "installing from existing dump: $dumpDir"
}
else {

if (-not (Test-Path -LiteralPath $DumperDll)) { throw "Dumper-7.dll missing at $DumperDll - run .\Get-Dumper7.ps1 first." }

# --- 1. global Dumper-7 profile -------------------------------------------------------
# Documented locations are <game exe dir>\Dumper-7.ini (per-game) or C:\Dumper-7\Dumper-7.ini
# (global). We use the global one so nothing is written into the game install.
New-Item -ItemType Directory -Path $DumpRoot -Force | Out-Null
$ini = @"
[Settings]
; Values under 1000 are seconds. Gives the engine time to finish coming up after injection.
SleepTimeout=$SleepTimeout
; F8 also triggers the dump early if you are watching the Dumper-7 console.
DumpKey=0x77
"@
Set-Content -LiteralPath (Join-Path $DumpRoot 'Dumper-7.ini') -Value $ini -Encoding ASCII
Info "wrote global profile $DumpRoot\Dumper-7.ini (SleepTimeout=$SleepTimeout, DumpKey=F8)"

# Remember what was already dumped so we can identify this run's folder.
$before = @(Get-ChildItem -LiteralPath $DumpRoot -Directory -ErrorAction SilentlyContinue | Select-Object -Expand FullName)

# --- 2. launch (or attach to) the game ------------------------------------------------
if ($AttachOnly) {
    $proc = @(Get-Process -Name 'A2-Win64-Shipping' -ErrorAction SilentlyContinue)[0]
    if (-not $proc) { throw "-AttachOnly given but A2-Win64-Shipping is not running." }
    Info "attaching to running pid $($proc.Id)"
} else {
    $exe = (Resolve-Path -LiteralPath $GameExe).Path
    Info "launching $exe"
    # -nullrhi: no renderer - nothing to draw for a dump, and it is how the server runs.
    # -nohmd/-nosound/-unattended/-nosplash: never block on a headset, audio device, or a dialog.
    $gameArgs = @('-nullrhi', '-nohmd', '-nosound', '-unattended', '-nosplash', '-log')
    # Redirect the child's stdout/stderr to files. Without this the game inherits our console
    # handles and any tooling that waits on those pipes blocks until the GAME exits.
    $logDir = Join-Path $PSScriptRoot 'logs'
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
    $stamp = Get-Date -Format yyyyMMdd-HHmmss
    $proc = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory (Split-Path -Parent $exe) -PassThru `
        -RedirectStandardOutput (Join-Path $logDir "A2-$stamp.out.log") `
        -RedirectStandardError  (Join-Path $logDir "A2-$stamp.err.log")
    Info "pid $($proc.Id); waiting ${InitWaitSeconds}s for engine init"
    for ($i = 0; $i -lt $InitWaitSeconds; $i++) {
        Start-Sleep -Seconds 1
        if ($proc.HasExited) {
            throw "Game exited during init with code $($proc.ExitCode). Launch it manually (headset/launcher) and re-run with -AttachOnly."
        }
    }
}

# --- 3. inject -------------------------------------------------------------------------
Info "injecting Dumper-7 (dump begins ${SleepTimeout}s later, or on F8)"
& (Join-Path $PSScriptRoot 'Inject.ps1') -ProcessId $proc.Id -DllPath $DumperDll

# --- 4. wait for the dump --------------------------------------------------------------
Info "waiting for a new dump under $DumpRoot (up to ${DumpTimeoutSeconds}s)"
$deadline = (Get-Date).AddSeconds($DumpTimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    $candidate = Get-ChildItem -LiteralPath $DumpRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { $before -notcontains $_.FullName } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    # CppGenerator runs first; the SDK is only usable once SDK.hpp exists, and MappingGenerator
    # runs right after it, so wait for both before declaring the dump finished.
    if ($candidate) {
        $haveSdk = Test-Path (Join-Path $candidate.FullName 'CppSDK\SDK.hpp')
        $haveMap = @(Get-ChildItem -LiteralPath (Join-Path $candidate.FullName 'Mappings') -Filter *.usmap -ErrorAction SilentlyContinue).Count -gt 0
        if ($haveSdk -and $haveMap) { $dumpDir = $candidate.FullName; break }
    }
    if ($proc.HasExited) { throw "Game exited (code $($proc.ExitCode)) before the dump finished. Check $DumpRoot for a partial dump." }
    Start-Sleep -Seconds 3
}
if (-not $dumpDir) { throw "No completed dump appeared within ${DumpTimeoutSeconds}s. Check $DumpRoot (and the Dumper-7 console on the game window)." }
Info "dump complete: $dumpDir"

}   # end: launch + inject + wait

# --- 5. install into the project -------------------------------------------------------
if ($NoInstall) {
    if ($proc -and -not $KeepGameRunning -and -not $AttachOnly -and -not $proc.HasExited) {
        Info "stopping game pid $($proc.Id)"
        Stop-Process -Id $proc.Id -Force
    }
    Info "-NoInstall: dump left at $dumpDir; project SDK untouched"
    return
}
$sdkSrc = Join-Path $dumpDir 'CppSDK'
if (-not $GameBuild) {
    # "5.4.2-22284+++A2+Release-A2" -> 22284
    if ((Split-Path -Leaf $dumpDir) -match '^\d+\.\d+\.\d+-(\d+)\+') { $GameBuild = $Matches[1] }
    else { throw "cannot infer the build number from '$dumpDir'; pass -GameBuild" }
}
$projDir = Join-Path $projDir "gamesdk\$GameBuild"
New-Item -ItemType Directory -Path $projDir -Force | Out-Null
Info "installing as game build $GameBuild -> $projDir"
$sdkDstDir = Join-Path $projDir 'SDK'
if (Test-Path -LiteralPath $sdkDstDir) {
    $backup = "$sdkDstDir.bak-$(Get-Date -Format yyyyMMdd-HHmmss)"
    Info "existing SDK moved aside -> $backup"
    Move-Item -LiteralPath $sdkDstDir -Destination $backup
}
# Copy the WHOLE CppSDK folder, per UsingTheSDK.md. Beyond SDK.hpp + SDK\ it carries
# PropertyFixup.hpp, UnrealContainers.hpp, UtfN.hpp, NameCollisions.inl and Assertions.inl,
# which SDK\Basic.hpp includes as '../<file>' - copying only SDK.hpp + SDK\ fails to compile.
Copy-Item -Path (Join-Path $sdkSrc '*') -Destination $projDir -Recurse -Force
$hdrCount = @(Get-ChildItem -LiteralPath $sdkDstDir -Filter *.hpp -Recurse).Count
$cppCount = @(Get-ChildItem -LiteralPath $sdkDstDir -Filter *.cpp -Recurse).Count
Info "installed SDK -> $projDir ($hdrCount headers, $cppCount sources)"

# Dumper-7's BP member-name de-duplication can still emit two identically named members in
# one struct (C2086 + every offset static_assert in that struct). Repair it in place.
& (Join-Path $PSScriptRoot 'Fix-SDK.ps1') -SdkDir $sdkDstDir

$mapDst = Join-Path $PSScriptRoot 'Mappings'
New-Item -ItemType Directory -Path $mapDst -Force | Out-Null
# -Path, not -LiteralPath: the wildcard has to expand. The dump folder name contains '+'
# characters, which are literal (not wildcard) so the path still resolves correctly.
Copy-Item -Path (Join-Path $dumpDir 'Mappings\*') -Destination $mapDst -Recurse -Force
Get-ChildItem -LiteralPath $mapDst -Filter *.usmap | ForEach-Object {
    Info "mappings -> $($_.FullName) ($([math]::Round($_.Length / 1KB)) KB)"
}

$provenance = @"
Dumper-7    : $(Get-Content (Join-Path $PSScriptRoot 'Dumper7\VERSION.txt') -ErrorAction SilentlyContinue)
Dump folder : $dumpDir
Game binary : $((Resolve-Path -LiteralPath $GameExe).Path)
Binary date : $((Get-Item (Resolve-Path -LiteralPath $GameExe).Path).LastWriteTime)
Generated   : $(Get-Date -Format s)
"@
Set-Content -LiteralPath (Join-Path $projDir 'SDK_SOURCE.txt') -Value $provenance

if ($proc -and -not $KeepGameRunning -and -not $AttachOnly -and -not $proc.HasExited) {
    Info "stopping game pid $($proc.Id)"
    Stop-Process -Id $proc.Id -Force
}

Info "done. Next: .\Build.ps1 -GameBuild $GameBuild   (plus a GameOffsets.h block for $GameBuild if it is new)"
