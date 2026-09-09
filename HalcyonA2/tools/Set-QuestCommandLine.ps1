<#
.SYNOPSIS
  Sets the UE command line on a connected Quest WITHOUT repacking or re-signing the APK.

.DESCRIPTION
  A UE Android build reads its command line from `assets/UECommandLine.txt` inside the APK, but it
  also honours an override file on the device:

      /sdcard/Android/data/<package>/files/UECommandLine.txt

  (`/UECommandLine.txt` is present in lib/arm64-v8a/libUnreal.so at +0x2752BA6, joined to a
  directory at runtime.)

  Using the override is much better than patching the APK:
    * the STORE-SIGNED build stays installed, so HorizonOS does not treat it as an unofficial app
      and does not gate the first launch behind the "unofficial app installed" dialog - which is
      exactly what made the repacked APK look like it "wasn't opening"
    * no uninstall, no re-sign, no OBB size juggling
    * changing the server is one line in a text file, pushed in a second

.EXAMPLE
  .\Set-QuestCommandLine.ps1 -Connect 192.168.1.29:7777
  .\Set-QuestCommandLine.ps1 -Connect 192.168.1.29:7777 -Extra "-httpproxy=192.168.1.29:8888"
  .\Set-QuestCommandLine.ps1 -Show
  .\Set-QuestCommandLine.ps1 -Clear
#>
[CmdletBinding()]
param(
    [string] $Connect,
    [string] $Extra,
    [string] $Package = 'com.AnotherAxiom.A2',
    [string] $Project = '-project="../../../A2/A2.uproject"',
    [switch] $Show,
    [switch] $Clear,
    [switch] $Launch
)
$ErrorActionPreference = 'Stop'
function Info($m) { Write-Host "[quest] $m" -ForegroundColor Cyan }
function Good($m) { Write-Host "[quest] $m" -ForegroundColor Green }

# adb: PATH first, then the copy Unity ships with its Android player.
$adbCmd = Get-Command adb -ErrorAction SilentlyContinue
$adb = if ($adbCmd) { $adbCmd.Source } else { $null }
if (-not $adb) {
    $adb = Get-ChildItem 'C:\Program Files\Unity\Hub\Editor' -Recurse -Filter adb.exe -ErrorAction SilentlyContinue |
           Select-Object -First 1 -Expand FullName
}
if (-not $adb) { throw "adb not found (not on PATH, and no Unity Android SDK copy)." }

# @() keeps this an array even with a single device - otherwise [0] indexes the first CHARACTER.
$devices = @(& $adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\sdevice$' })
if ($devices.Count -eq 0) { throw "No device in 'adb devices' - is the headset plugged in and USB debugging allowed?" }
Info "adb: $adb"
Info "device: $(($devices[0] -split '\s+')[0])"

$dir    = "/sdcard/Android/data/$Package/files"
$remote = "$dir/UECommandLine.txt"

if ($Show) {
    Info "current override:"
    & $adb shell cat $remote
    return
}

if ($Clear) {
    & $adb shell rm -f $remote
    Good "removed $remote (the APK's built-in command line applies again)"
    return
}

if (-not $Connect -and -not $Extra) { throw "Pass -Connect <ip:port> and/or -Extra, or use -Show / -Clear." }

$parts = @($Project)
if ($Connect) { $parts += "-connectToServerByIPAndPort=$Connect" }
if ($Extra)   { $parts += $Extra }
$line = ($parts -join ' ')

# UE wants a plain one-line file; write it without a BOM.
$local = Join-Path $env:TEMP 'UECommandLine.txt'
[System.IO.File]::WriteAllText($local, $line + "`n", (New-Object System.Text.UTF8Encoding($false)))

& $adb shell mkdir -p $dir | Out-Null
& $adb push $local $remote | Out-Null
Good "pushed: $line"

Info "readback:"
& $adb shell cat $remote

if ($Launch) {
    Info "launching..."
    & $adb shell am start -n "$Package/com.epicgames.unreal.GameActivity" | Out-Null
    Good "launched - if this is a re-signed build, accept the 'unofficial app' dialog in the headset once"
}
