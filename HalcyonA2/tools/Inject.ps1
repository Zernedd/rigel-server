<#
.SYNOPSIS
  Injects a DLL into a running process (CreateRemoteThread + LoadLibraryW).
.EXAMPLE
  .\Inject.ps1 -ProcessName A2-Win64-Shipping -DllPath .\Dumper7\Dumper-7.dll
  .\Inject.ps1 -ProcessId 1234 -DllPath ..\build\x64\Release\HalcyonA2.dll
#>
[CmdletBinding(DefaultParameterSetName = 'ByName')]
param(
    [Parameter(Mandatory, ParameterSetName = 'ByName')]  [string] $ProcessName,
    [Parameter(Mandatory, ParameterSetName = 'ById')]    [int]    $ProcessId,
    [Parameter(Mandatory)]                               [string] $DllPath,
    [int] $TimeoutMs = 30000
)

$ErrorActionPreference = 'Stop'

$dll = (Resolve-Path -LiteralPath $DllPath).Path
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { throw "DLL not found: $DllPath" }

if ($PSCmdlet.ParameterSetName -eq 'ByName') {
    $procs = @(Get-Process -Name ($ProcessName -replace '\.exe$', '') -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) { throw "No running process named '$ProcessName'." }
    if ($procs.Count -gt 1) { throw "Multiple '$ProcessName' processes ($($procs.Id -join ', ')); pass -ProcessId." }
    $ProcessId = $procs[0].Id
}

Add-Type -Namespace HalcyonInject -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true)]
public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr addr, IntPtr size, uint type, uint protect);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool VirtualFreeEx(IntPtr h, IntPtr addr, IntPtr size, uint type);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool WriteProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr written);
// CharSet matters: GetModuleHandleW takes UTF-16, GetProcAddress always takes ANSI.
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
public static extern IntPtr GetModuleHandleW(string name);
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Ansi)]
public static extern IntPtr GetProcAddress(IntPtr mod, string name);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern IntPtr CreateRemoteThread(IntPtr h, IntPtr sa, IntPtr stack, IntPtr start, IntPtr param, uint flags, IntPtr tid);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern uint WaitForSingleObject(IntPtr h, uint ms);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool GetExitCodeThread(IntPtr h, out uint code);
[DllImport("kernel32.dll", SetLastError=true)]
public static extern bool CloseHandle(IntPtr h);
'@

$PROCESS_ALL_ACCESS = 0x1F0FFF
$MEM_COMMIT_RESERVE = 0x3000
$MEM_RELEASE        = 0x8000
$PAGE_READWRITE     = 0x04

$h = [HalcyonInject.Native]::OpenProcess($PROCESS_ALL_ACCESS, $false, $ProcessId)
if ($h -eq [IntPtr]::Zero) { throw "OpenProcess($ProcessId) failed: $([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message). Run elevated?" }

$remote = [IntPtr]::Zero
try {
    # UTF-16 path + terminator, written into the target so LoadLibraryW can read it there.
    $bytes = [Text.Encoding]::Unicode.GetBytes($dll + "`0")
    $remote = [HalcyonInject.Native]::VirtualAllocEx($h, [IntPtr]::Zero, [IntPtr]$bytes.Length, $MEM_COMMIT_RESERVE, $PAGE_READWRITE)
    if ($remote -eq [IntPtr]::Zero) { throw "VirtualAllocEx failed." }

    $written = [IntPtr]::Zero
    if (-not [HalcyonInject.Native]::WriteProcessMemory($h, $remote, $bytes, [IntPtr]$bytes.Length, [ref]$written)) {
        throw "WriteProcessMemory failed."
    }

    # kernel32 is loaded at the same base in every process on a given boot, so our
    # LoadLibraryW address is valid in the target.
    $k32 = [HalcyonInject.Native]::GetModuleHandleW("kernel32.dll")
    if ($k32 -eq [IntPtr]::Zero) { throw "GetModuleHandleW(kernel32.dll) failed." }
    $load = [HalcyonInject.Native]::GetProcAddress($k32, "LoadLibraryW")
    if ($load -eq [IntPtr]::Zero) { throw "GetProcAddress(LoadLibraryW) failed." }

    $thread = [HalcyonInject.Native]::CreateRemoteThread($h, [IntPtr]::Zero, [IntPtr]::Zero, $load, $remote, 0, [IntPtr]::Zero)
    if ($thread -eq [IntPtr]::Zero) { throw "CreateRemoteThread failed (architecture mismatch? both must be x64)." }

    $wait = [HalcyonInject.Native]::WaitForSingleObject($thread, $TimeoutMs)
    $code = 0
    [void][HalcyonInject.Native]::GetExitCodeThread($thread, [ref]$code)
    [void][HalcyonInject.Native]::CloseHandle($thread)

    if ($wait -ne 0) { Write-Warning "LoadLibraryW did not return within ${TimeoutMs}ms (it may still be running)." }
    elseif ($code -eq 0) { throw "LoadLibraryW returned NULL - the DLL failed to load in the target (missing dependency, or a DllMain that returned FALSE)." }
    else { Write-Host "[inject] loaded $(Split-Path -Leaf $dll) into pid $ProcessId (module base 0x$('{0:X}' -f $code))" -ForegroundColor Green }
}
finally {
    if ($remote -ne [IntPtr]::Zero) { [void][HalcyonInject.Native]::VirtualFreeEx($h, $remote, [IntPtr]::Zero, $MEM_RELEASE) }
    [void][HalcyonInject.Native]::CloseHandle($h)
}
