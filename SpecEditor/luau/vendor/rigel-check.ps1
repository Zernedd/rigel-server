<#
rigel-check.ps1 -- Rigel-specific checks for Spec Editor Luau scripts, shown in VS Code's Problems panel.

luau-lsp (IntelliSense) knows the language and the game's API, but not the traps that only exist in the
game itself. This catches those. VS Code runs it for you in the background (.vscode\tasks.json, task
"Rigel: check scripts", started when the folder opens); every save is re-checked.

  powershell -File tools\rigel-check.ps1            check once
  powershell -File tools\rigel-check.ps1 -Watch     keep checking as files change

Output: <file>:<line>:<col>: warning: <message>  (the format the task's problem matcher reads).
#>
param([string]$Root = '', [switch]$Watch)
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }

# Typed locals become the object's editor properties. Only these types can be properties.
function Test-PropertyType([string]$t) {
    $t = $t.Trim()
    if ($t.EndsWith('?')) { $t = $t.Substring(0, $t.Length - 1).Trim() }
    if ($t -eq 'number' -or $t -eq 'string' -or $t -eq 'boolean') { return $true }
    return ($t -match '^[A-Za-z_][A-Za-z0-9_]*Component$')
}

function Get-LineCol([string]$s, [int]$at) {
    $line = 1; $last = -1
    for ($q = 0; $q -lt $at; $q++) { if ($s[$q] -eq "`n") { $line++; $last = $q } }
    return @($line, ($at - $last))
}

function Test-IdChar([char]$c) { return [char]::IsLetterOrDigit($c) -or $c -eq '_' }

# [[ ... ]] / [==[ ... ]==]: index just past the close, or -1 when $i doesn't open one.
function Skip-LongBracket([string]$s, [int]$i) {
    if ($i -ge $s.Length -or $s[$i] -ne '[') { return -1 }
    $j = $i + 1; $eq = 0
    while ($j -lt $s.Length -and $s[$j] -eq '=') { $eq++; $j++ }
    if ($j -ge $s.Length -or $s[$j] -ne '[') { return -1 }
    $close = ']' + ('=' * $eq) + ']'
    $e = $s.IndexOf($close, $j + 1)
    if ($e -lt 0) { return $s.Length }
    return $e + $close.Length
}

function Test-File([string]$path) {
    $out = New-Object System.Collections.Generic.List[string]
    try { $s = [IO.File]::ReadAllText($path) } catch { return $out }
    $s = $s -replace "`r`n", "`n"
    $n = $s.Length
    $i = 0
    while ($i -lt $n) {
        $c = $s[$i]
        if ($c -eq '-' -and $i + 1 -lt $n -and $s[$i + 1] -eq '-') {                 # comment
            $lb = Skip-LongBracket $s ($i + 2)
            if ($lb -ge 0) { $i = $lb; continue }
            $e = $s.IndexOf("`n", $i); if ($e -lt 0) { $e = $n }
            $i = $e; continue
        }
        if ($c -eq '"' -or $c -eq "'" -or $c -eq '`') {                               # string
            $j = $i + 1
            while ($j -lt $n -and $s[$j] -ne $c -and $s[$j] -ne "`n") { if ($s[$j] -eq '\') { $j++ }; $j++ }
            $i = $j + 1; continue
        }
        if ($c -eq '[') { $lb = Skip-LongBracket $s $i; if ($lb -ge 0) { $i = $lb; continue } }
        if ($c -eq '.' -and $i + 20 -le $n -and $s.Substring($i, 20) -eq '.defaultEnabledValue') {
            $lc = Get-LineCol $s $i
            $out.Add((("{0}:{1}:{2}: warning: Rigel: defaultEnabledValue is in the type definitions but errors when the script runs. " +
                      "Use the object's Game data (IsEnabled) in the editor instead.") -f $path, $lc[0], $lc[1]))
        }
        if ($c -eq 'l' -and $i + 5 -lt $n -and $s.Substring($i, 5) -eq 'local' -and ($i -eq 0 -or -not (Test-IdChar $s[$i - 1])) -and
            -not (Test-IdChar $s[$i + 5])) {
            $j = $i + 5
            while ($true) {                                                        # each `name [: type]` of the statement
                while ($j -lt $n -and ($s[$j] -eq ' ' -or $s[$j] -eq "`t")) { $j++ }
                $n0 = $j
                while ($j -lt $n -and (Test-IdChar $s[$j])) { $j++ }
                if ($j -eq $n0) { break }
                $name = $s.Substring($n0, $j - $n0)
                if ($name -eq 'function') { break }
                while ($j -lt $n -and ($s[$j] -eq ' ' -or $s[$j] -eq "`t")) { $j++ }
                if ($j -lt $n -and $s[$j] -eq '<') { $e = $s.IndexOf('>', $j); if ($e -lt 0) { break }; $j = $e + 1 }
                while ($j -lt $n -and ($s[$j] -eq ' ' -or $s[$j] -eq "`t")) { $j++ }
                if ($j -lt $n -and $s[$j] -eq ':') {
                    $colon = $j; $k = $j + 1; $depth = 0
                    while ($k -lt $n) {
                        $d = $s[$k]
                        if ($d -eq '{' -or $d -eq '(' -or $d -eq '<') { $depth++ }
                        elseif ($d -eq '}' -or $d -eq ')' -or $d -eq '>') { if ($depth -gt 0) { $depth-- } }
                        elseif ($depth -eq 0 -and ($d -eq '=' -or $d -eq ',' -or $d -eq "`n" -or $d -eq ';')) { break }
                        elseif ($depth -eq 0 -and $d -eq '-' -and $k + 1 -lt $n -and $s[$k + 1] -eq '-') { break }
                        $k++
                    }
                    $type = ($s.Substring($colon + 1, $k - $colon - 1) -replace '\s+', ' ').Trim()
                    $lc = Get-LineCol $s $n0
                    if (-not (Test-PropertyType $type)) {
                        $shown = if ($type.Length -gt 50) { $type.Substring(0, 47) + '...' } else { $type }
                        $out.Add((("{0}:{1}:{2}: warning: Rigel: '{3}: {4}' - a typed local becomes an editor property of the object, and " +
                                  "this type can't be one (it crashed the server and every player). The server removes the type when the " +
                                  "script is sent, so it still works; write 'local {3} = ...' to silence this.") -f $path, $lc[0], $lc[1], $name, $shown))
                    }
                    elseif ($type -notmatch 'Component\??$' -and $k -lt $n -and $s[$k] -eq '=') {
                        # number/string/boolean with a value: the property replaces it (nil unless set in the editor).
                        $e = $s.IndexOf("`n", $k); if ($e -lt 0) { $e = $n }
                        $init = $s.Substring($k + 1, $e - $k - 1)
                        $cut = $init.IndexOf('--'); if ($cut -ge 0) { $init = $init.Substring(0, $cut) }
                        $init = $init.Trim()
                        if ($init -and $init -ne 'nil') {
                            $out.Add((("{0}:{1}:{2}: warning: Rigel: '{3}: {4}' is an editor property, so it starts as nil in the game - " +
                                      "not {5}. Drop the type ('local {3} = {5}') to keep the value.") -f $path, $lc[0], $lc[1], $name, $type, $init))
                        }
                    }
                    $j = $k
                }
                while ($j -lt $n -and ($s[$j] -eq ' ' -or $s[$j] -eq "`t")) { $j++ }
                if ($j -lt $n -and $s[$j] -eq ',') { $j++; continue }
                break
            }
            $i = $j; continue
        }
        $i++
    }
    # Line rules (comments stripped): ball spawns from scripts, BeginPlay in game mode code.
    $isMode = $path -match '\\GameModes\\'
    $ln = 0
    foreach ($raw in ($s -split "`n")) {
        $ln++
        $line = $raw -replace '--.*$', ''
        if ($line -match '\b(spawn(Ball|BallWithParameters|PersonalBall|SinglePersonalBall|HeartBall)|server_SpawnBall)\s*\(') {
            $out.Add((("{0}:{1}:1: warning: Rigel: don't spawn balls from a script - it runs on every machine and each makes its own " +
                      "ball nobody else sees. Give the spawner the game mode 'Ball spawner' role and call Rigel.resetBalls().") -f $path, $ln))
        }
        if ($isMode -and $line -match '^\s*function\s+BeginPlay\s*\(') {
            $out.Add((("{0}:{1}:1: warning: Rigel: game mode code must not define BeginPlay (the generated controller owns it). " +
                      "Use the hooks: OnLobby, OnCountdown, OnRoundStart, OnRoundEnd, OnScore, OnTime, OnTeamChanged.") -f $path, $ln))
        }
    }
    return $out
}

function Invoke-Scan {
    'rigel-check: checking'
    $count = 0
    Get-ChildItem -Path $Root -Recurse -Filter *.luau -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notmatch '\\(game-scripts|node_modules|\.git)\\' } |
        ForEach-Object { foreach ($l in (Test-File $_.FullName)) { $l; $count++ } }
    "rigel-check: done ($count issue(s))"
}

Invoke-Scan
if (-not $Watch) { return }
$w = New-Object IO.FileSystemWatcher $Root, '*.luau'
$w.IncludeSubdirectories = $true
while ($true) {
    $r = $w.WaitForChanged([IO.WatcherChangeTypes]::All, 2000)
    if ($r.TimedOut) { continue }
    Start-Sleep -Milliseconds 250                                                   # let the editor finish writing
    Invoke-Scan
}
