<#
.SYNOPSIS
  Gets a freshly launched A2 client past its first-run consent screens and (optionally)
  connects it to a server through the UE console.

.DESCRIPTION
  A brand new client profile shows "Terms Of Service" then "Privacy Policy", each with an
  Exit / Accept pair. Until they are accepted the game sits on those panels and the console
  cannot travel anywhere. This clicks Accept, then drives A2ConsoleUnlock's ` console to run
  "open <server>".

  Only needed once per client folder - the acceptance is saved in that build's Saved\Config.

.EXAMPLE
  .\Enter-Game.ps1 -ProcessId 1234 -Connect 127.0.0.1:7777
  .\Enter-Game.ps1 -ProcessId 1234 -AcceptOnly
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [int] $ProcessId,
    [string] $Connect,
    [switch] $AcceptOnly,
    [int] $Width  = 1300,
    [int] $Height = 780
)
$ErrorActionPreference = 'Stop'

Add-Type -Namespace Ent -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int ht,bool r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n);
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f,uint dx,uint dy,uint d,IntPtr e);
'@

$proc = Get-Process -Id $ProcessId -ErrorAction Stop
$h = $proc.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "pid $ProcessId has no main window yet" }

# Park the window at the origin so the Accept button is at a known place.
[void][Ent.Win]::ShowWindow($h, 9)
[void][Ent.Win]::MoveWindow($h, 0, 0, $Width, $Height, $true)

# Accept button sits in the lower middle-right of the consent panel.
$ax = [int]($Width * 0.639)
$ay = [int]($Height * 0.719)

Write-Host "[enter] clicking Accept on the consent screens..."
for ($i = 1; $i -le 4; $i++) {
    [void][Ent.Win]::BringWindowToTop($h); [void][Ent.Win]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 500
    [void][Ent.Win]::SetCursorPos($ax, $ay); Start-Sleep -Milliseconds 300
    [Ent.Win]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 90
    [Ent.Win]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero)
    Start-Sleep -Seconds 4
}

if ($AcceptOnly -or -not $Connect) { Write-Host "[enter] done (no connect requested)"; return }

Write-Host "[enter] connecting to $Connect via the ` console..."
$send = Join-Path $PSScriptRoot 'Send-GameKeys.ps1'
& $send -ProcessId $ProcessId -Key Tilde | Out-Null
Start-Sleep -Milliseconds 1200
& $send -ProcessId $ProcessId -Type "open $Connect"
Write-Host "[enter] sent: open $Connect"
