# Drives the Spec Editor UI with real mouse/keyboard input for tests/ui.txt: click, drag, key.
# Coordinates come from the script's "screenpos" log line (screen pixels).
param(
    [Parameter(Mandatory)] [int] $ProcessId,
    [ValidateSet('click', 'drag', 'move', 'key')] [string] $Action = 'click',
    [int] $X, [int] $Y, [int] $X2, [int] $Y2,
    [int] $Steps = 30, [int] $StepMs = 16,
    [string] $Key
)
$ErrorActionPreference = 'Stop'
Add-Type -Namespace Win -Name Inp -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint f, IntPtr e);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
'@
$p = Get-Process -Id $ProcessId
[Win.Inp]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 300
$LDOWN = 0x2; $LUP = 0x4
switch ($Action) {
    'move'  { [Win.Inp]::SetCursorPos($X, $Y) | Out-Null }
    'click' {
        [Win.Inp]::SetCursorPos($X, $Y) | Out-Null; Start-Sleep -Milliseconds 120
        [Win.Inp]::mouse_event($LDOWN, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 60
        [Win.Inp]::mouse_event($LUP, 0, 0, 0, [IntPtr]::Zero)
    }
    'drag'  {
        [Win.Inp]::SetCursorPos($X, $Y) | Out-Null; Start-Sleep -Milliseconds 200
        [Win.Inp]::mouse_event($LDOWN, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 80
        for ($i = 1; $i -le $Steps; $i++) {
            $cx = [int]($X + ($X2 - $X) * $i / $Steps); $cy = [int]($Y + ($Y2 - $Y) * $i / $Steps)
            [Win.Inp]::SetCursorPos($cx, $cy) | Out-Null; Start-Sleep -Milliseconds $StepMs
        }
        Start-Sleep -Milliseconds 100
        [Win.Inp]::mouse_event($LUP, 0, 0, 0, [IntPtr]::Zero)
    }
    'key'   {
        $vk = [byte][char]$Key.ToUpper()
        [Win.Inp]::keybd_event($vk, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 60
        [Win.Inp]::keybd_event($vk, 0, 2, [IntPtr]::Zero)
    }
}
"ok $Action"
