param(
    [Parameter(Mandatory = $true)]
    [string]$Binary,

    [Parameter(Mandatory = $true)]
    [string]$Arch,

    [string]$OpenBlasRoot = "",
    [string]$VcpkgRoot = "",
    [string]$CmakeOptions = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$binaryPath = if ([System.IO.Path]::IsPathRooted($Binary)) { $Binary } else { Join-Path $repoRoot $Binary }
if (!(Test-Path $binaryPath)) {
    throw "binary is missing: $binaryPath"
}

$shortSha = (& git -C $repoRoot rev-parse --short=12 HEAD).Trim()
$commitSha = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($env:MRADM_VERSION) {
    $version = $env:MRADM_VERSION
} elseif ($env:GITHUB_REF_TYPE -eq "tag" -and $env:GITHUB_REF_NAME) {
    $version = $env:GITHUB_REF_NAME
} else {
    try {
        $version = (& git -C $repoRoot describe --tags --exact-match 2>$null).Trim()
        if ([string]::IsNullOrWhiteSpace($version)) {
            $version = "0.0.0-dev.$shortSha"
        }
    } catch {
        $version = "0.0.0-dev.$shortSha"
    }
}

$distDir = Join-Path $repoRoot "dist"
$packageName = "mradm-$version-windows-$Arch"
$packageRoot = Join-Path $distDir $packageName
$binDir = Join-Path $packageRoot "bin"
$archive = Join-Path $distDir "$packageName.zip"
$checksum = "$archive.sha256"

Remove-Item -Recurse -Force $packageRoot, $archive, $checksum -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $binDir | Out-Null

Copy-Item $binaryPath -Destination (Join-Path $binDir "mradm.exe") -Force
Copy-Item (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $packageRoot "LICENSE") -Force
Copy-Item (Join-Path $repoRoot "docs\THIRD_PARTY_LICENSES.md") -Destination (Join-Path $packageRoot "THIRD_PARTY_NOTICES.md") -Force

$dllSearchDirs = @()
if ($OpenBlasRoot -and (Test-Path $OpenBlasRoot)) {
    $dllSearchDirs += Join-Path $OpenBlasRoot "bin"
}
if ($VcpkgRoot -and (Test-Path $VcpkgRoot)) {
    $dllSearchDirs += Join-Path $VcpkgRoot "installed\x64-windows\bin"
}
$dllSearchDirs += Split-Path -Parent $binaryPath

foreach ($dir in $dllSearchDirs | Select-Object -Unique) {
    if (Test-Path $dir) {
        Get-ChildItem -Path $dir -Filter "*.dll" -Recurse -ErrorAction SilentlyContinue |
            ForEach-Object { Copy-Item $_.FullName -Destination $binDir -Force }
    }
}

$depsFile = Join-Path $packageRoot "DEPENDENCIES.txt"
if ($env:VCVARS64 -and (Test-Path $env:VCVARS64)) {
    $depsCmd = Join-Path $env:TEMP "mradm-release-deps.cmd"
    @"
@echo off
call "$env:VCVARS64"
dumpbin /dependents "$binaryPath"
"@ | Set-Content -Path $depsCmd -Encoding ASCII
    cmd /c $depsCmd | Out-File -FilePath $depsFile -Encoding utf8
} else {
    "dumpbin dependency scan unavailable; VCVARS64 was not set." | Set-Content -Path $depsFile -Encoding utf8
}

$buildInfo = @(
    "name: MacinRender ADM Core",
    "binary: mradm.exe",
    "version: $version",
    "commit: $commitSha",
    "platform: windows",
    "arch: $Arch",
    "built_at_utc: $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))",
    "cmake_options: $(if ($CmakeOptions) { $CmakeOptions } else { 'not recorded' })",
    "dependency_policy: Windows packages include mradm.exe plus copied runtime DLLs from the OpenBLAS, vcpkg, and build output directories."
)
$buildInfo | Set-Content -Path (Join-Path $packageRoot "BUILD_INFO.txt") -Encoding utf8

Push-Location $distDir
try {
    Compress-Archive -Path $packageName -DestinationPath $archive -Force
    $hash = (Get-FileHash -Algorithm SHA256 -Path $archive).Hash.ToLowerInvariant()
    "$hash  $(Split-Path -Leaf $archive)" | Set-Content -Path $checksum -Encoding ASCII
} finally {
    Pop-Location
}

Write-Host $archive
Write-Host $checksum
