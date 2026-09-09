<#
.SYNOPSIS
  Post-processes a freshly generated Dumper-7 SDK to fix duplicate member names.

.DESCRIPTION
  Dumper-7 de-duplicates Blueprint member names (Foo, Foo_0, Foo_1 ...) but the mangling can
  still land two members of the same struct on the same C++ identifier - most often in
  *_parameters.hpp for BP ExecuteUbergraph structs. That is a hard C2086 "redefinition", and
  because one member then vanishes, every offset static_assert in that struct fails too.

  This renames the SECOND and later occurrences of a repeated member name to <name>_dupN, in
  both places the name appears:
    SDK\*.hpp        the struct body
    Assertions.inl   the matching static_assert (offsetof + message), matched by position so
                     the assert keeps checking the offset it was generated for

  Types and offsets are never touched, so the layout the asserts verify is unchanged.
  Idempotent: re-running finds nothing to do. Dump-A2.ps1 runs it after every dump.

.EXAMPLE
  .\Fix-SDK.ps1
  .\Fix-SDK.ps1 -WhatIfOnly     # report the collisions, change nothing
#>
[CmdletBinding()]
param(
    [string] $SdkDir = (Join-Path $PSScriptRoot '..\HalcyonA2\SDK'),
    [string] $AssertionsFile = (Join-Path $PSScriptRoot '..\HalcyonA2\Assertions.inl'),
    [switch] $WhatIfOnly
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $SdkDir)) { throw "SDK folder not found: $SdkDir" }

# A member declaration always carries a trailing offset comment:
#     <type...>  <name>;   // 0x0090(0x0008)(...)
$memberRe = [regex] '^(?<lead>\s+.*?\s)(?<name>[A-Za-z_]\w*)(?<tail>;\s*//\s*0x[0-9A-Fa-f]+.*)$'
# "struct FFoo final", "struct alignas(0x08) FBar final", "struct SDK_ALIGN(0x01) FBaz : public FQux".
# Anchored at column 0 with no leading whitespace: a type declaration is never indented,
# whereas a member of class-pointer type ("\tclass UInputAction* Foo;") always is.
$typeRe   = [regex] '^(?:struct|class)\s+(?:(?:alignas|SDK_ALIGN)\(\s*0x[0-9A-Fa-f]+\s*\)\s+)?(?<name>\w+)'

# Collisions found in the headers: key "<Struct>|<Member>" -> list of new names by occurrence
# (index 0 = the 2nd occurrence, which is the first one we rename).
$collisions = @{}
$filesChanged = 0
$renames = 0

foreach ($file in Get-ChildItem -LiteralPath $SdkDir -Filter *.hpp -Recurse) {
    $lines = [System.IO.File]::ReadAllLines($file.FullName)
    $seen = @{}
    $currentType = ''
    $dirty = $false

    for ($i = 0; $i -lt $lines.Length; $i++) {
        $line = $lines[$i]

        $t = $typeRe.Match($line)
        if ($t.Success) { $currentType = $t.Groups['name'].Value; $seen = @{}; continue }

        $m = $memberRe.Match($line)
        if (-not $m.Success) { continue }

        $name = $m.Groups['name'].Value
        if ($seen.ContainsKey($name)) {
            $seen[$name]++
            $newName = "{0}_dup{1}" -f $name, $seen[$name]
            Write-Host "[fix-sdk] $($file.Name):$($i + 1)  $currentType::$name -> $newName" -ForegroundColor Yellow
            $renames++

            $key = "$currentType|$name"
            if (-not $collisions.ContainsKey($key)) { $collisions[$key] = [System.Collections.Generic.List[string]]::new() }
            $collisions[$key].Add($newName)

            if (-not $WhatIfOnly) {
                $lines[$i] = $m.Groups['lead'].Value + $newName + $m.Groups['tail'].Value
                $dirty = $true
            }
        }
        else { $seen[$name] = 1 }
    }

    if ($dirty) {
        [System.IO.File]::WriteAllLines($file.FullName, $lines)
        $filesChanged++
    }
}

# --- empty-base placeholder pads --------------------------------------------------------
# A UE ScriptStruct with no properties is reported as 1 byte, and Dumper-7 sometimes fills
# that with `uint8 Pad_0[0x1];`. A struct that DERIVES from it then loses empty-base
# optimisation: its first member shifts from 0 to the base's aligned size and every size /
# offset static_assert in the derived struct fails (22284: FObjectWrapperFragment ->
# FMassActorFragment). The game's real layout is the EBO one, so drop the placeholder
# whenever a derived struct places its first member at offset 0. An empty C++ struct is
# still sizeof 1, so the base's own assert keeps passing.
$padOnlyRe = [regex] '^\s+uint8\s+Pad_0\[0x1\];\s*//\s*0x0000\(0x0001\).*$'
$derivedRe = [regex] '^(?:struct|class)\s+(?:SDK_ALIGN\(\w+\)\s+|alignas\(\w+\)\s+)?(?<name>\w+)\b[^\n]*:\s*public\s+(?<base>\w+)'
$headers = @(Get-ChildItem -LiteralPath $SdkDir -Filter *.hpp -Recurse)

# 1) which structs consist of nothing but the placeholder pad?
$padOnly = @{}
foreach ($file in $headers) {
    $lines = [System.IO.File]::ReadAllLines($file.FullName)
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $t = $typeRe.Match($lines[$i])
        if (-not $t.Success) { continue }
        # body = lines between "{" and "};"
        $j = $i + 1
        while ($j -lt $lines.Length -and $lines[$j] -notmatch '^\{') { $j++ }
        $k = $j + 1; $members = @()
        while ($k -lt $lines.Length -and $lines[$k] -notmatch '^\};') { if ($lines[$k] -match ';\s*//\s*0x') { $members += $lines[$k] }; $k++ }
        if ($members.Count -eq 1 -and $padOnlyRe.IsMatch($members[0])) {
            $padOnly[$t.Groups['name'].Value] = @{ File = $file.FullName; Line = $k }
        }
    }
}

# 2) is any of them used as a base whose derived struct starts at offset 0?
$bogus = @{}
if ($padOnly.Count -gt 0) {
    foreach ($file in $headers) {
        $lines = [System.IO.File]::ReadAllLines($file.FullName)
        for ($i = 0; $i -lt $lines.Length; $i++) {
            $d = $derivedRe.Match($lines[$i])
            if (-not $d.Success -or -not $padOnly.ContainsKey($d.Groups['base'].Value)) { continue }
            for ($k = $i + 1; $k -lt [Math]::Min($i + 6, $lines.Length); $k++) {
                if ($lines[$k] -match ';\s*//\s*0x0000\(') { $bogus[$d.Groups['base'].Value] = $d.Groups['name'].Value; break }
                if ($lines[$k] -match '^\};') { break }
            }
        }
    }
}

$padsRemoved = 0
foreach ($base in $bogus.Keys) {
    $f = $padOnly[$base].File
    $lines = [System.IO.File]::ReadAllLines($f)
    $changed = $false
    for ($i = 0; $i -lt $lines.Length; $i++) {
        $t = $typeRe.Match($lines[$i])
        if (-not $t.Success -or $t.Groups['name'].Value -ne $base) { continue }
        for ($k = $i + 1; $k -lt [Math]::Min($i + 8, $lines.Length); $k++) {
            if ($padOnlyRe.IsMatch($lines[$k])) {
                Write-Host "[fix-sdk] $([System.IO.Path]::GetFileName($f)):$($k + 1)  $base is an empty base used by $($bogus[$base]); dropping placeholder Pad_0[0x1]" -ForegroundColor Yellow
                if (-not $WhatIfOnly) {
                    # remove the pad line and a now-dangling "public:" before it
                    $keep = [System.Collections.Generic.List[string]]::new()
                    for ($m = 0; $m -lt $lines.Length; $m++) {
                        if ($m -eq $k) { continue }
                        if ($m -eq $k - 1 -and $lines[$m] -match '^public:\s*$') { continue }
                        $keep.Add($lines[$m])
                    }
                    $lines = $keep.ToArray(); $changed = $true
                }
                $padsRemoved++
                break
            }
        }
        break
    }
    if ($changed) { [System.IO.File]::WriteAllLines($f, $lines) }
}
if ($padsRemoved -gt 0) { $renames += 0 }   # counted separately below

# --- keep Assertions.inl in step -------------------------------------------------------
$assertsPatched = 0
if ($collisions.Count -gt 0 -and (Test-Path -LiteralPath $AssertionsFile)) {
    $aLines = [System.IO.File]::ReadAllLines($AssertionsFile)
    $offsetOfRe = [regex] 'offsetof\(\s*(?<type>\w+)\s*,\s*(?<member>\w+)\s*\)'
    $counts = @{}

    for ($i = 0; $i -lt $aLines.Length; $i++) {
        $m = $offsetOfRe.Match($aLines[$i])
        if (-not $m.Success) { continue }

        $key = "$($m.Groups['type'].Value)|$($m.Groups['member'].Value)"
        if (-not $collisions.ContainsKey($key)) { continue }

        # Nth sighting of this (type, member) assert: the 1st keeps the original name, the
        # 2nd takes the first replacement, and so on - the same order as the struct body.
        if (-not $counts.ContainsKey($key)) { $counts[$key] = 0 }
        $counts[$key]++
        $replacementIndex = $counts[$key] - 2      # 1st sighting -> -1 (leave alone)
        if ($replacementIndex -lt 0) { continue }
        if ($replacementIndex -ge $collisions[$key].Count) { continue }

        $member = $m.Groups['member'].Value
        $newName = $collisions[$key][$replacementIndex]
        Write-Host "[fix-sdk] Assertions.inl:$($i + 1)  $member -> $newName" -ForegroundColor Yellow
        $assertsPatched++
        if (-not $WhatIfOnly) {
            # \b so Temp_real_Variable_1 never matches inside Temp_real_Variable_1_0.
            $aLines[$i] = [regex]::Replace($aLines[$i], "\b$([regex]::Escape($member))\b", $newName)
        }
    }

    if ($assertsPatched -gt 0 -and -not $WhatIfOnly) { [System.IO.File]::WriteAllLines($AssertionsFile, $aLines) }
}

if ($renames -eq 0 -and $padsRemoved -eq 0) {
    Write-Host "[fix-sdk] no duplicate member names or bogus base pads found - SDK is clean." -ForegroundColor Green
} elseif ($WhatIfOnly) {
    Write-Host "[fix-sdk] $renames member collision(s) + $assertsPatched assert(s) would change (nothing written)." -ForegroundColor Cyan
} else {
    Write-Host "[fix-sdk] renamed $renames member(s) across $filesChanged file(s), patched $assertsPatched assert(s), dropped $padsRemoved bogus base pad(s)." -ForegroundColor Green
}
