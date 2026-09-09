<#
.SYNOPSIS
  Sends real keyboard input to a running A2 client window (SendInput, not SendKeys).

.DESCRIPTION
  Used to drive the test clients without a human at the keyboard:
    - open the UE console (A2ConsoleUnlock binds ` / Tilde) and type a command
    - press F1 (A2PlayerControl: leave spectator)
    - hold WASD for a while (A2PlayerControl: fly)

  SendInput posts into the global input queue, so GetAsyncKeyState - which is what
  A2PlayerControl samples - sees the keys. The window is focused first anyway, because the
  UE console itself reads normal Windows messages and needs focus.

.EXAMPLE
  .\Send-GameKeys.ps1 -Pid 1234 -Console 'open 127.0.0.1:7777'
  .\Send-GameKeys.ps1 -Pid 1234 -Key F1
  .\Send-GameKeys.ps1 -Pid 1234 -Hold W -Seconds 3
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [int] $ProcessId,
    [string] $Console,
    [string] $Type,
    [string] $Key,
    [string] $Hold,
    [double] $Seconds = 2.0
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace Win -Name Input -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
[DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr hWnd);
[DllImport("user32.dll")] public static extern IntPtr SetActiveWindow(IntPtr hWnd);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
[DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
[DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();

// Windows refuses SetForegroundWindow from a process that does not already own the
// foreground. Attaching our input queue to the current foreground thread lifts that
// restriction for the duration of the call - the standard workaround.
public static bool Focus(IntPtr hWnd) {
    IntPtr fg = GetForegroundWindow();
    uint fgThread = GetWindowThreadProcessId(fg, IntPtr.Zero);
    uint me = GetCurrentThreadId();
    if (fgThread != me) AttachThreadInput(me, fgThread, true);
    ShowWindow(hWnd, 9);
    BringWindowToTop(hWnd);
    SetForegroundWindow(hWnd);
    SetActiveWindow(hWnd);
    SetFocus(hWnd);
    if (fgThread != me) AttachThreadInput(me, fgThread, false);
    return GetForegroundWindow() == hWnd;
}
[DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] pInputs, int cbSize);

[StructLayout(LayoutKind.Sequential)]
public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
[StructLayout(LayoutKind.Explicit, Size=40)]
public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }

public const uint INPUT_KEYBOARD = 1;
public const uint KEYEVENTF_KEYUP = 0x0002;
public const uint KEYEVENTF_UNICODE = 0x0004;

public static void Key(ushort vk, bool up) {
    INPUT[] i = new INPUT[1];
    i[0].type = INPUT_KEYBOARD;
    i[0].ki.wVk = vk;
    i[0].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
}
// KEYEVENTF_UNICODE synthesises VK_PACKET, and UE's console text field ignores it - the
// characters never appear. Send the REAL virtual key (plus shift state) that produces the
// character on the current layout instead, so Windows generates an ordinary WM_CHAR.
public static void Char(char c) {
    short vs = VkKeyScanW(c);
    if (vs == -1) return;
    ushort vk = (ushort)(vs & 0xFF);
    bool shift = (vs & 0x0100) != 0;
    if (shift) Key(0x10, false);          // VK_SHIFT down
    Key(vk, false);
    Key(vk, true);
    if (shift) Key(0x10, true);
}
[DllImport("user32.dll")] public static extern short VkKeyScanW(char ch);
'@

$proc = Get-Process -Id $ProcessId -ErrorAction Stop
$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { throw "pid $ProcessId has no main window yet" }

$focused = $false
for ($i = 0; $i -lt 5 -and -not $focused; $i++) {
    $focused = [Win.Input]::Focus($hwnd)
    Start-Sleep -Milliseconds 400
}
if (-not $focused) { Write-Warning "could not bring pid $ProcessId to the foreground; keys may not reach it" }
Start-Sleep -Milliseconds 500

$VK = @{
    'Tilde' = 0xC0; 'Return' = 0x0D; 'F1' = 0x70; 'F2' = 0x71
    'W' = 0x57; 'A' = 0x41; 'S' = 0x53; 'D' = 0x44
    'Space' = 0x20; 'Shift' = 0x10; 'Ctrl' = 0x11
}

if ($Console) {
    [Win.Input]::Key($VK['Tilde'], $false); Start-Sleep -Milliseconds 60
    [Win.Input]::Key($VK['Tilde'], $true);  Start-Sleep -Milliseconds 800
    foreach ($c in $Console.ToCharArray()) { [Win.Input]::Char($c); Start-Sleep -Milliseconds 25 }
    Start-Sleep -Milliseconds 300
    [Win.Input]::Key($VK['Return'], $false); Start-Sleep -Milliseconds 60
    [Win.Input]::Key($VK['Return'], $true)
    Write-Host "[keys] pid $ProcessId console: $Console"
}

if ($Type) {
    # Console already open: just type and submit. (-Console presses ` first, which would
    # TOGGLE an already-open console shut.)
    foreach ($c in $Type.ToCharArray()) { [Win.Input]::Char($c); Start-Sleep -Milliseconds 25 }
    Start-Sleep -Milliseconds 300
    [Win.Input]::Key($VK['Return'], $false); Start-Sleep -Milliseconds 60
    [Win.Input]::Key($VK['Return'], $true)
    Write-Host "[keys] pid $ProcessId typed: $Type"
}

if ($Key) {
    if (-not $VK.ContainsKey($Key)) { throw "unknown key '$Key'" }
    [Win.Input]::Key($VK[$Key], $false); Start-Sleep -Milliseconds 80
    [Win.Input]::Key($VK[$Key], $true)
    Write-Host "[keys] pid $ProcessId pressed $Key"
}

if ($Hold) {
    if (-not $VK.ContainsKey($Hold)) { throw "unknown key '$Hold'" }
    $vk = $VK[$Hold]
    $end = (Get-Date).AddSeconds($Seconds)
    # Re-send the keydown periodically: a single down would be enough for GetAsyncKeyState,
    # but repeating it survives the window losing and regaining focus mid-test.
    while ((Get-Date) -lt $end) { [Win.Input]::Key($vk, $false); Start-Sleep -Milliseconds 40 }
    [Win.Input]::Key($vk, $true)
    Write-Host "[keys] pid $ProcessId held $Hold for $Seconds s"
}
