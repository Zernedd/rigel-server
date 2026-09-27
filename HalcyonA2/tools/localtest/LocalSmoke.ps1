<#
LocalSmoke.ps1 -- the local test a server change must pass before it goes live (RigelCrashWatch fix sessions run it
before staging or swapping a DLL). Runs the freshly built payload on a local test server with real clients:

  1. local server (HalcyonA2\tools\Start-Server.ps1 -Config server.specedit.psd1, the build in build\22284\x64\Release)
  2. a mock player (PlayerNovBuild: stock game + UE4SS controls) and the Spec Editor (dev build, MCP bridge)
  3. -Level opened (default BallBattle); the mock walks onto MiniJakeball's team 1 (seated in the ball sim)
  4. kickoff and a goal
  5. a second stock player joins while the mock is on a team (the join hold)
  6. the level is closed and re-opened with the mock seated (the unseat)
  7. -Repro <script.py|script.ps1>: your own reproduction of the crash (lines starting FAIL fail the run)
  8. every game process still alive, and no new crash report

Prints PASS/FAIL per step and exits 0 only if all passed. Every wait is capped (the whole run takes ~10 minutes).
Only processes started from this repo are stopped -- never the user's own Rigel editor.
  LocalSmoke.ps1 [-Level BallBattle] [-Repro path] [-KeepRunning] [-SkipGame]
#>
param([string]$Level = 'BallBattle', [string]$Repro = '', [switch]$KeepRunning, [switch]$SkipGame)
$ErrorActionPreference = 'Continue'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = (Resolve-Path (Join-Path $Here '..\..\..')).Path
$Work = Join-Path $env:TEMP 'RigelLocalTest'
$Results = [System.Collections.Generic.List[string]]::new()
function Step([string]$kind, [string]$msg) { $l = "$kind $msg"; $Results.Add($l); Write-Host $l }
function Stop-TestGames {
    Get-Process A2-Win64-Shipping, CrashReportClient -EA SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($Root, [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object { Stop-Process -Id $_.Id -Force }
}
function Start-Game([string]$exe, [string[]]$gameArgs, [string]$tempDir) {
    New-Item -ItemType Directory -Force $tempDir | Out-Null
    $oT = $env:TEMP; $oTm = $env:TMP
    $env:TEMP = $tempDir; $env:TMP = $tempDir
    try { return Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -ArgumentList $gameArgs -PassThru }
    finally { $env:TEMP = $oT; $env:TMP = $oTm }
}
function Wait-Pawn($proc, [string]$dir, [int]$cap) {
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $cap) {
        Start-Sleep 5
        if ($proc.HasExited) { return "EXITED (code $($proc.ExitCode))" }
        $me = Select-String -Path "$dir\A2PlayerControl.log" -Pattern 'BP_VRPawn_C.*<-- me' -EA SilentlyContinue | Select-Object -Last 1
        if ($me -and $me.Line -notmatch 'pos=\(0, 0, 0\)') { return 'PAWN' }
        Set-Content "$dir\A2PlayerControl.cmd" 'pawns' -EA SilentlyContinue
    }
    return 'TIMEOUT'
}
function LT([string[]]$a) {
    $env:RIGEL_MCP_PORT = $script:Port; $env:MOCK_DIR = "$Work\mock"
    $o = & python "$Here\lt.py" @a 2>&1
    foreach ($l in $o) { if ($l -match '^(PASS|FAIL|INFO) ') { Step $Matches[1] $l.Substring(5) } }
}

Remove-Item $Work -Recurse -Force -EA SilentlyContinue
New-Item -ItemType Directory -Force $Work | Out-Null
Stop-TestGames
Start-Sleep 3
$crashDir = "$env:LOCALAPPDATA\A2\Saved\Crashes"
$crash0 = (Get-ChildItem $crashDir -Directory -EA SilentlyContinue | Measure-Object).Count
$srvLog = "$env:TEMP\HalcyonA2.log"
$logMark = if (Test-Path $srvLog) { (Get-Item $srvLog).Length } else { 0 }
$dll = "$Root\HalcyonA2\build\22284\x64\Release\HalcyonA2.dll"
Step 'INFO' "payload under test: $dll ($((Get-Item $dll).Length) bytes, built $((Get-Item $dll).LastWriteTime))"

# 1. server
Push-Location "$Root\HalcyonA2\tools"
$so = & .\Start-Server.ps1 -Config server.specedit.psd1 2>&1 | Out-String
Pop-Location
$server = Get-Process A2-Win64-Shipping -EA SilentlyContinue | Where-Object { $_.Path -like "$Root\Nov15\*" } | Select-Object -First 1
if (-not $server) { Step 'FAIL' "local server did not start: $(($so -split "`n" | Select-Object -Last 3) -join ' | ')"; Stop-TestGames; exit 1 }
Step 'PASS' "local server up (pid $($server.Id))"

# 2. mock player + editor
$mockExe = "$Root\PlayerNovBuild\A2\Binaries\Win64\A2-Win64-Shipping.exe"
$mock = Start-Game $mockExe @('-connectToServerByIPAndPort=127.0.0.1:7777', '-windowed', '-ResX=960', '-ResY=540', '-WinX=980', '-WinY=0', '-nosound', '-log', '-HalcyonAutoSpawn') "$Work\mock"
Start-Sleep 30
$r = Wait-Pawn $mock "$Work\mock" 150
if ($r -ne 'PAWN') { Step 'FAIL' "mock player: $r" } else { Step 'PASS' "mock player joined (pid $($mock.Id))" }
$edBin = "$Root\SpecEditorBuild\A2\Binaries\Win64"
Copy-Item -Force "$Root\SpecEditor\mod\dsound.dll" "$edBin\dsound.dll" -EA SilentlyContinue
$editor = Start-Game "$edBin\A2-Win64-Shipping.exe" @('-windowed', '-nohmd', '-ResX=1280', '-ResY=720', '-SpecEditConnect=127.0.0.1:7777', "-SpecEditScript=$Root\SpecEditor\tests\t_mcp_idle_long.txt") "$Work\editor"
# (the exe it was started as may hand over to another process: find the editor by its folder, not by that handle)
$edLog = "$Work\editor\spec_editor.log"
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt 200) {
    Start-Sleep 3
    if ((Test-Path $edLog) -and (Select-String -Path $edLog -Pattern 'MCPIDLE ready' -SimpleMatch -Quiet)) { break }
}
$editor = Get-Process A2-Win64-Shipping -EA SilentlyContinue | Where-Object { $_.Path -like "$edBin\*" } | Select-Object -First 1
$script:Port = (Get-Content "$Work\editor\rigel_mcp_port.txt" -EA SilentlyContinue | Select-Object -First 1)
if (-not $script:Port -or -not $editor) {
    Step 'FAIL' "editor did not come up (process: $(if ($editor) { $editor.Id } else { 'none' }); log: $(if (Test-Path $edLog) { 'present' } else { 'missing' }))"
    if (-not $KeepRunning) { Stop-TestGames }; exit 1
}
Step 'PASS' "editor up (MCP port $($script:Port))"
Start-Sleep 8

# 3-6. the scenario
LT @('open', $Level)
if (-not $SkipGame) {
    LT @('onteam')
    LT @('kickoff')
    LT @('goal')
}
# The joiner is a second editor with its join guard OFF (-NoJoinGuard): the stock client's path through the team list,
# and -- unlike a second PlayerNovBuild -- a different identity. Two PlayerNovBuild copies on one PC share one EOS
# account, and the second login kills the first player's session (a test artifact that crashed the mock, 2026-09-27).
$null = Start-Game "$edBin\A2-Win64-Shipping.exe" @('-windowed', '-nohmd', '-ResX=640', '-ResY=360', '-SpecEditConnect=127.0.0.1:7777', '-NoJoinGuard', "-SpecEditScript=$Root\SpecEditor\tests\t_mcp_idle_long.txt") "$Work\joiner"
$jLog = "$Work\joiner\spec_editor.log"
$t0 = Get-Date; $r = 'TIMEOUT'
while (((Get-Date) - $t0).TotalSeconds -lt 200) {
    Start-Sleep 3
    if ((Test-Path $jLog) -and (Select-String -Path $jLog -Pattern 'MCPIDLE ready' -SimpleMatch -Quiet)) { $r = 'JOINED'; break }
}
$joiner = Get-Process A2-Win64-Shipping -EA SilentlyContinue | Where-Object { $_.Path -like "$edBin\*" -and $_.Id -ne $editor.Id } | Select-Object -First 1
if (-not $joiner) { $r = "no second client process ($r)" }
elseif ($r -eq 'JOINED') { Start-Sleep 25; $joiner.Refresh(); if ($joiner.HasExited) { $r = "died after joining (code $($joiner.ExitCode))" } }
Step $(if ($r -eq 'JOINED') { 'PASS' } else { 'FAIL' }) "a second (unguarded) client joined while the mock was on a team: $r"
LT @('reload', $Level)
$unseat = $false
try {
    $fs = [IO.File]::Open($srvLog, 'Open', 'Read', 'ReadWrite'); $fs.Seek([Math]::Min($logMark, $fs.Length), 'Begin') | Out-Null
    $unseat = (New-Object IO.StreamReader($fs)).ReadToEnd().Contains('taken off the teams'); $fs.Close()
} catch {}
if (-not $SkipGame) { Step $(if ($unseat) { 'PASS' } else { 'FAIL' }) 'players were taken off the teams before the reload' }

# 7. the crash's own repro
if ($Repro) {
    $env:RIGEL_MCP_PORT = $script:Port; $env:MOCK_DIR = "$Work\mock"
    $ro = if ($Repro -like '*.py') { & python $Repro 2>&1 } else { & powershell -NoProfile -ExecutionPolicy Bypass -File $Repro 2>&1 }
    $rf = @($ro | Where-Object { $_ -match '^FAIL' })
    Step $(if ($rf.Count) { 'FAIL' } else { 'PASS' }) "repro $Repro ($(@($ro).Count) line(s)$(if ($rf.Count) { ": $($rf[0])" }))"
}

# 8. nothing died
Start-Sleep 5
foreach ($p in @(@{ n = 'server'; p = $server }, @{ n = 'mock player'; p = $mock }, @{ n = 'editor'; p = $editor }, @{ n = 'second player'; p = $joiner })) {
    $p.p.Refresh()
    Step $(if ($p.p.HasExited) { 'FAIL' } else { 'PASS' }) "$($p.n) still running$(if ($p.p.HasExited) { " -- exited with code $($p.p.ExitCode)" })"
}
$crash1 = (Get-ChildItem $crashDir -Directory -EA SilentlyContinue | Measure-Object).Count
Step $(if ($crash1 -eq $crash0) { 'PASS' } else { 'FAIL' }) "crash reports: $crash0 -> $crash1"

if (-not $KeepRunning) { Stop-TestGames }
$fails = @($Results | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host ''
Write-Host $(if ($fails) { "LOCAL SMOKE: FAIL ($fails step(s))" } else { 'LOCAL SMOKE: PASS' })
Set-Content (Join-Path $Work 'result.txt') ($Results + $(if ($fails) { "FAIL ($fails)" } else { 'PASS' }))
exit $(if ($fails) { 1 } else { 0 })
