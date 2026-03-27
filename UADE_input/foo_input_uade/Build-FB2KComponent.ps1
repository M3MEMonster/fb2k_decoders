<#
.SYNOPSIS
    Builds the foobar2000 component package.
.DESCRIPTION
    Executed during post-build. Copies platform binaries and optional extra files,
    then creates a dual-arch .fb2k-component package layout:
      root = Win32 files
      x64/ = x64 files
#>

[CmdletBinding()]
param(
    [parameter(Mandatory)][string] $TargetName,
    [parameter(Mandatory)][string] $TargetFileName,
    [parameter(Mandatory)][string] $Platform,
    [parameter(Mandatory)][string] $OutputPath
)

#Requires -Version 7.2

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ProjectDir = $PSScriptRoot
$OutRoot = Join-Path $ProjectDir "..\\out\\$TargetName"
$OutRoot = [IO.Path]::GetFullPath($OutRoot)
$OutX64 = Join-Path $OutRoot 'x64'

if (!(Test-Path $OutRoot)) { $null = New-Item -ItemType Directory -Path $OutRoot -Force }
if (!(Test-Path $OutX64)) { $null = New-Item -ItemType Directory -Path $OutX64 -Force }

$OutputPathNormalized = $OutputPath.Trim().Trim('"').TrimEnd('\', '/')

if ([IO.Path]::IsPathRooted($OutputPathNormalized)) {
    $targetSource = Join-Path $OutputPathNormalized $TargetFileName
} else {
    $targetSource = Join-Path $ProjectDir (Join-Path $OutputPathNormalized $TargetFileName)
}
$targetSource = [IO.Path]::GetFullPath($targetSource)

if (!(Test-Path $targetSource)) {
    # Backward-compat fallback for older projects that may still output to $(ProjectDir)\$(Configuration)\
    $fallback = Join-Path $ProjectDir (Join-Path (Split-Path -Leaf (Split-Path $OutputPathNormalized -Parent)) $TargetFileName)
    if (Test-Path $fallback) {
        $targetSource = [IO.Path]::GetFullPath($fallback)
    }
}

if (!(Test-Path $targetSource)) {
    throw "Target binary not found: $targetSource"
}

$extraCommon = @(
)
$extraX64 = @(

)

# UADE data must be deployed as 'uade/' (find_uade_data_path searches for 'uade/eagleplayer.conf')
$uadeDataSrc = Join-Path $ProjectDir '3rdParty\UADE\data'

function Copy-Optional([string[]]$paths, [string]$dest) {
    foreach($rel in $paths) {
        if ([string]::IsNullOrWhiteSpace($rel)) { continue }
        $src = Join-Path $ProjectDir $rel
        if (Test-Path $src) {
            if (Test-Path $src -PathType Container) {
                Copy-Item $src -Destination $dest -Recurse -Force
            } else {
                Copy-Item $src -Destination $dest -Force
            }
        }
    }
}

if ($Platform -eq 'x64') {
    if (Test-Path $targetSource) { Copy-Item $targetSource -Destination $OutX64 -Force }
    Copy-Optional -paths $extraCommon -dest $OutRoot
    Copy-Optional -paths $extraX64 -dest $OutX64
}
elseif ($Platform -eq 'Win32') {
    if (Test-Path $targetSource) { Copy-Item $targetSource -Destination $OutRoot -Force }
    Copy-Optional -paths $extraCommon -dest $OutRoot
}
else {
    Write-Host "Unknown platform: $Platform"
    exit 0
}

# Copy UADE data into 'uade/' subfolder (not 'data/') to match find_uade_data_path() expectations
if (Test-Path $uadeDataSrc) {
    $uadeDest = Join-Path $OutRoot 'uade'
    if (-not (Test-Path $uadeDest)) { $null = New-Item -ItemType Directory -Path $uadeDest -Force }
    Copy-Item -Path (Join-Path $uadeDataSrc '*') -Destination $uadeDest -Recurse -Force
}

if (Test-Path (Join-Path $OutRoot '*')) {
    $pkg = Join-Path (Split-Path $OutRoot -Parent) "$TargetName.fb2k-component"
    if (Test-Path $pkg) { Remove-Item $pkg -Force }
    Compress-Archive -Force -Path (Join-Path $OutRoot '*') -DestinationPath $pkg
}
