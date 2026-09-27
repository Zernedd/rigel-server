<#
RigelCrashWatch -- when a live Rigel game server crashes, collect the evidence from the VPS and start a Claude Code
session (a fork of the conversation that built this server) in the background to find and fix the crash point.

  CrashWatch.ps1 install   register the scheduled task (checks every 2 minutes while you are logged on) and turn it on
  CrashWatch.ps1 off       turn it off (the task is disabled AND a DISABLED file makes any run a no-op)
  CrashWatch.ps1 on        turn it back on
  CrashWatch.ps1 status    on/off, last check, crashes seen, fix sessions started
  CrashWatch.ps1 run       one check now (what the task runs)
  CrashWatch.ps1 uninstall remove the scheduled task (state and bundles stay in %LOCALAPPDATA%\RigelCrashWatch)

Double-click CrashWatch-OFF.cmd / CrashWatch-ON.cmd in this folder for the same off / on.
Fix sessions: `claude agents` lists them, `claude attach <id>` joins one, `claude stop <id>` stops one.
#>
param([ValidateSet('run', 'install', 'uninstall', 'on', 'off', 'status')][string]$Command = 'status')
$ErrorActionPreference = 'Continue'

$TaskName   = 'RigelCrashWatch'
$Here       = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo       = (Resolve-Path (Join-Path $Here '..\..\..')).Path          # OrionDriftStuff
$Dir        = Join-Path $env:LOCALAPPDATA 'RigelCrashWatch'
$StateFile  = Join-Path $Dir 'state.json'
$ConfigFile = Join-Path $Dir 'config.json'
$Disabled   = Join-Path $Dir 'DISABLED'
$LogFile    = Join-Path $Dir 'watch.log'
$Inbox      = Join-Path $Dir 'inbox'
New-Item -ItemType Directory -Force $Dir, $Inbox | Out-Null

# settings (config.json overrides; written with the defaults on first use)
$cfg = [ordered]@{
    vps           = 'Administrator@13.140.41.197'
    sshKey        = (Join-Path $env:USERPROFILE '.ssh\vps_ed25519')
    session       = 'e1127066-f64a-456c-9a3d-24070d7a41d5'   # the conversation to fork for each fix
    claude        = (Join-Path $env:USERPROFILE '.local\bin\claude.exe')
    permission    = 'auto'
    maxPerDay     = 4                                         # fix sessions started in any 24 h
    lockHours     = 2                                         # one fix session at a time (assumed busy this long)
    dedupeHours   = 24                                        # the same crash signature starts one session a day
}
if (Test-Path $ConfigFile) { (Get-Content $ConfigFile -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $cfg[$_.Name] = $_.Value } }
else { $cfg | ConvertTo-Json | Set-Content $ConfigFile }

function Log([string]$m) { $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $m"; Add-Content $LogFile $line; Write-Host $line }
function Load-State {
    if (Test-Path $StateFile) { try { return Get-Content $StateFile -Raw | ConvertFrom-Json } catch {} }
    return [pscustomobject]@{ offset = -1; probeHash = ''; lastCheck = ''; crashes = @() }
}
function Save-State($s) { $s | ConvertTo-Json -Depth 6 | Set-Content $StateFile }
# NOT named Ssh: function names ignore case, so `& ssh` inside it called itself (call depth overflow on every check)
function Invoke-Vps([string]$cmd) {
    & ssh.exe -i $cfg.sshKey -o BatchMode=yes -o ConnectTimeout=20 $cfg.vps $cmd 2>&1 | Where-Object { $_ -notmatch 'post-quantum|store now, decrypt later|may need to be upgraded' }
}

switch ($Command) {
'install' {
    $ps = (Get-Command powershell.exe).Source
    $act = New-ScheduledTaskAction -Execute $ps -Argument "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$($MyInvocation.MyCommand.Path)`" run" -WorkingDirectory $Repo
    $trg = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) -RepetitionInterval (New-TimeSpan -Minutes 2)
    $set = New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 10) -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    $pri = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName $TaskName -Action $act -Trigger $trg -Settings $set -Principal $pri -Force | Out-Null
    Remove-Item $Disabled -EA SilentlyContinue
    Log "installed: task '$TaskName' checks every 2 minutes while you are logged on"
}
'uninstall' { Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -EA SilentlyContinue; Log "uninstalled the scheduled task" }
'off' {
    Set-Content $Disabled "turned off $(Get-Date -Format s)"
    Disable-ScheduledTask -TaskName $TaskName -EA SilentlyContinue | Out-Null
    Log "OFF: no checks, no new fix sessions (running ones: claude agents / claude stop <id>)"
}
'on' {
    Remove-Item $Disabled -EA SilentlyContinue
    Enable-ScheduledTask -TaskName $TaskName -EA SilentlyContinue | Out-Null
    Log "ON"
}
'status' {
    $s = Load-State
    $t = Get-ScheduledTask -TaskName $TaskName -EA SilentlyContinue
    "RigelCrashWatch: " + $(if (Test-Path $Disabled) { 'OFF' } elseif (-not $t) { 'not installed' } elseif ($t.State -eq 'Disabled') { 'OFF (task disabled)' } else { 'ON' })
    "last check: $($s.lastCheck)   crashes recorded: $(@($s.crashes).Count)"
    @($s.crashes) | Select-Object -Last 8 | ForEach-Object { "  $($_.time)  pid $($_.pid)  $($_.status)  $($_.signature)  $($_.session)" }
    "log: $LogFile   bundles: $Inbox"
}
'run' {
    if (Test-Path $Disabled) { return }
    $s = Load-State
    # the probe lives on the VPS; re-send it when it changed
    $probe = Join-Path $Here 'vps_probe.ps1'
    $hash = (Get-FileHash $probe -Algorithm SHA256).Hash
    if ($s.probeHash -ne $hash) {
        & scp.exe -i $cfg.sshKey -o BatchMode=yes -o ConnectTimeout=20 $probe "$($cfg.vps):C:/Env/crashwatch_probe.ps1" 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) { $s.probeHash = $hash } else { Log "could not copy the probe to the VPS"; return }
    }
    $out = Invoke-Vps "powershell -NoProfile -File C:/Env/crashwatch_probe.ps1 -Since $($s.offset)"
    $json = @($out) | Where-Object { $_ -like '{*' } | Select-Object -Last 1
    if (-not $json) { Log "probe gave no answer: $((@($out) | Select-Object -Last 2) -join ' | ')"; return }
    $r = $json | ConvertFrom-Json
    $s.lastCheck = (Get-Date -Format s)
    if ($r.note -eq 'baseline') { $s.offset = $r.offset; Save-State $s; Log "baseline set at agent.log byte $($r.offset)"; return }
    $crashes = [System.Collections.Generic.List[object]]::new(); @($s.crashes) | Where-Object { $_ } | ForEach-Object { $crashes.Add($_) }
    foreach ($e in @($r.events)) {
        # signature: where it faulted + the first two game frames (first fault block), else the exit code
        $sig = "exit $($e.code) (no fault block)"
        $first = @($e.faults) | Select-Object -First 1
        if ($first) {
            $rip = [regex]::Match($first, 'rip in (\S+) \+0x([0-9A-Fa-f]+)')
            $frames = [regex]::Matches($first, 'stack\[\+0x[0-9A-F]+\] GAME \+0x([0-9A-F]+)') | Select-Object -First 2 | ForEach-Object { "GAME+0x$($_.Groups[1].Value)" }
            $sig = (@($(if ($rip.Success) { "$($rip.Groups[1].Value)+0x$($rip.Groups[2].Value)" })) + @($frames) | Where-Object { $_ }) -join ' < '
        }
        $id = "$((Get-Date).ToString('yyyyMMdd-HHmmss'))_pid$($e.pid)"
        $bdir = Join-Path $Inbox $id
        New-Item -ItemType Directory -Force $bdir | Out-Null
        $b = @("# Server crash $id", "", "- pid: $($e.pid)", "- when (VPS time): $($e.time)", "- how: $($e.how)", "- exit code: $($e.code)",
               "- signature: $sig", "- UE log: $($e.ueLog)", "", "## Payload fault blocks (HalcyonA2.log, this pid)", "")
        if (@($e.faults).Count) { foreach ($f in @($e.faults)) { $b += '```'; $b += $f; $b += '```'; $b += '' } } else { $b += '(none logged for this pid -- an older payload, or the process died without a fault)'; $b += '' }
        $b += '## UE log tail'; $b += '```'; $b += $e.ueTail; $b += '```'; $b += ''; $b += '## Agent log'; $b += '```'; $b += $e.agent; $b += '```'
        $bundle = Join-Path $bdir 'bundle.md'
        $b -join "`n" | Set-Content $bundle -Encoding UTF8
        $crashes.Add([pscustomobject]@{ id = $id; pid = $e.pid; time = $e.time; code = $e.code; signature = $sig; bundle = $bundle; status = 'pending'; session = ''; launchedAt = '' })
        Log "CRASH: server pid $($e.pid) $($e.how) at $($e.time) (code $($e.code)) -- $sig -- $bundle"
    }
    $s.offset = $r.offset
    # start at most one fix session per check, within the limits
    $now = Get-Date
    $launched = @($crashes | Where-Object { $_.launchedAt })
    $busy  = $launched | Where-Object { $now - [datetime]$_.launchedAt -lt (New-TimeSpan -Hours $cfg.lockHours) }
    $today = @($launched | Where-Object { $now - [datetime]$_.launchedAt -lt (New-TimeSpan -Hours 24) }).Count
    foreach ($c in @($crashes | Where-Object { $_.status -eq 'pending' })) {
        $dup = $launched | Where-Object { $_.signature -eq $c.signature -and $now - [datetime]$_.launchedAt -lt (New-TimeSpan -Hours $cfg.dedupeHours) } | Select-Object -First 1
        if ($dup) { $c.status = "same crash as $($dup.id) (its session covers it)"; continue }
        if ($busy) { continue }                                   # wait for the running session
        if ($today -ge $cfg.maxPerDay) { $c.status = 'pending (daily limit reached)'; continue }
        $prompt = (Get-Content (Join-Path $Here 'crashfix_prompt.md') -Raw).Replace('{BUNDLE}', $c.bundle).Replace('{SIGNATURE}', $c.signature).
                  Replace('{PID}', "$($c.pid)").Replace('{TIME}', "$($c.time)").Replace('{CODE}', "$($c.code)")
        Push-Location $Repo
        try {
            $res = & $cfg.claude --bg --resume $cfg.session --fork-session --permission-mode $cfg.permission --name "crashfix pid $($c.pid)" $prompt 2>&1 | Out-String
        } finally { Pop-Location }
        $c.status = 'fix session started'; $c.launchedAt = (Get-Date -Format s); $c.session = ($res -split "`r?`n" | Where-Object { $_ } | Select-Object -Last 1)
        Log "started a fix session for $($c.id): $($res.Trim())"
        break
    }
    $s.crashes = @($crashes | Select-Object -Last 100)
    Save-State $s
}
}
