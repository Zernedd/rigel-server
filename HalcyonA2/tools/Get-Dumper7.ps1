<#
.SYNOPSIS
  Downloads the latest Dumper-7 release DLL into tools\Dumper7\.
  Re-run to update; the DLL is the only artifact the dump needs.
#>
[CmdletBinding()]
param([string] $Tag = 'latest')

$ErrorActionPreference = 'Stop'
$dest = Join-Path $PSScriptRoot 'Dumper7'
New-Item -ItemType Directory -Path $dest -Force | Out-Null

$api = if ($Tag -eq 'latest') { 'https://api.github.com/repos/Encryqed/Dumper-7/releases/latest' }
       else { "https://api.github.com/repos/Encryqed/Dumper-7/releases/tags/$Tag" }

$rel = Invoke-RestMethod -Uri $api -Headers @{ 'User-Agent' = 'HalcyonA2' }
$asset = $rel.assets | Where-Object { $_.name -eq 'Dumper-7.dll' } | Select-Object -First 1
if (-not $asset) { throw "Release $($rel.tag_name) has no Dumper-7.dll asset." }

$out = Join-Path $dest 'Dumper-7.dll'
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $out -UseBasicParsing
Write-Host "[dumper7] $($rel.tag_name) -> $out ($([math]::Round((Get-Item $out).Length/1KB)) KB)" -ForegroundColor Green
Set-Content -LiteralPath (Join-Path $dest 'VERSION.txt') -Value "$($rel.tag_name)  ($($rel.published_at))"
