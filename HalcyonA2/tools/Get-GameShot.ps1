<#
.SYNOPSIS
  Screenshots a running A2 client window to a PNG.
.EXAMPLE
  .\Get-GameShot.ps1 -ProcessId 1234 -Path C:\temp\shot.png
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [int] $ProcessId,
    [Parameter(Mandatory)] [string] $Path
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type -Namespace Win -Name Shot -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int n);
'@

$p = Get-Process -Id $ProcessId -ErrorAction Stop
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "pid $ProcessId has no main window" }

# The game renders with D3D12, so PrintWindow returns black; grab the screen region the
# window occupies instead, which means it has to be on top.
[void][Win.Shot]::ShowWindow($h, 9)
[void][Win.Shot]::BringWindowToTop($h)
[void][Win.Shot]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 600

$r = New-Object Win.Shot+RECT
[void][Win.Shot]::GetWindowRect($h, [ref]$r)
$w = $r.R - $r.L; $ht = $r.B - $r.T
if ($w -le 0 -or $ht -le 0) { throw "bad window rect for pid $ProcessId" }

$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
$g.Dispose()
$bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "[shot] pid $ProcessId ($w x $ht) -> $Path"
