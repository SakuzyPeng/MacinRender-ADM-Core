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

trap {
    if ($_.Exception.Message) {
        Write-Host "package-windows-cli-release failure: $($_.Exception.Message)"
    }
    if ($_.InvocationInfo.PositionMessage) {
        Write-Host $_.InvocationInfo.PositionMessage
    }
    if ($_.ScriptStackTrace) {
        Write-Host $_.ScriptStackTrace
    }
    throw
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

function Test-ExistingPath {
    param([AllowNull()][object]$Path)

    $text = [string]$Path
    if ([string]::IsNullOrWhiteSpace($text)) {
        return $false
    }
    return Test-Path -LiteralPath $text
}

function Resolve-ExistingPath {
    param([AllowNull()][object]$Path)

    $text = [string]$Path
    if ([string]::IsNullOrWhiteSpace($text)) {
        return $null
    }
    if (!(Test-Path -LiteralPath $text)) {
        return $null
    }
    return (Resolve-Path -LiteralPath $text).Path
}

function Remove-IfExists {
    param([AllowNull()][object[]]$Paths)

    foreach ($path in $Paths) {
        $resolved = Resolve-ExistingPath $path
        if ($resolved) {
            Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

$binaryPath = if ([System.IO.Path]::IsPathRooted($Binary)) { $Binary } else { Join-Path $repoRoot $Binary }
if (!(Test-ExistingPath $binaryPath)) {
    throw "binary is missing: $binaryPath"
}
$binaryPath = Resolve-ExistingPath $binaryPath

function Test-SystemDll {
    param([Parameter(Mandatory = $true)][string]$Name)

    $lower = $Name.ToLowerInvariant()
    if ($lower -like "api-ms-win-*.dll" -or $lower -like "ext-ms-win-*.dll") {
        return $true
    }

    $systemDlls = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    @(
        "advapi32.dll", "bcrypt.dll", "bcryptprimitives.dll", "cfgmgr32.dll",
        "combase.dll", "comctl32.dll", "comdlg32.dll", "coremessaging.dll",
        "crypt32.dll", "cryptbase.dll", "cryptsp.dll", "d2d1.dll", "d3d11.dll",
        "d3d12.dll", "d3dcompiler_47.dll", "dbghelp.dll", "dcomp.dll",
        "dnsapi.dll", "dwrite.dll", "dwmapi.dll", "dxgi.dll", "fontsub.dll",
        "gdi32.dll", "gdi32full.dll", "gdiplus.dll", "hid.dll", "imm32.dll",
        "iphlpapi.dll", "kernel32.dll", "kernelbase.dll", "mf.dll",
        "mfplat.dll", "mfreadwrite.dll", "mpr.dll", "msvcp_win.dll", "msvcrt.dll",
        "ncrypt.dll", "netapi32.dll", "ntdll.dll", "ole32.dll", "oleacc.dll",
        "oleaut32.dll", "opengl32.dll", "powrprof.dll", "propsys.dll", "psapi.dll",
        "rpcrt4.dll", "sechost.dll", "setupapi.dll", "shcore.dll", "shell32.dll",
        "shlwapi.dll", "textinputframework.dll", "ucrtbase.dll", "user32.dll",
        "userenv.dll", "usp10.dll", "uxtheme.dll", "version.dll", "winhttp.dll",
        "wininet.dll", "winmm.dll", "winspool.drv", "wintypes.dll",
        "windowscodecs.dll", "wintrust.dll", "ws2_32.dll"
    ) | ForEach-Object { [void]$systemDlls.Add($_) }
    return $systemDlls.Contains($Name)
}

function Get-DependentDllNames {
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "dependency scan path is empty"
    }
    if (!($env:VCVARS64 -and (Test-ExistingPath $env:VCVARS64))) {
        throw "VCVARS64 must be set so dumpbin can scan Windows release dependencies"
    }

    $depsCmd = Join-Path ([System.IO.Path]::GetTempPath()) ("mradm-release-deps-" + [System.Guid]::NewGuid().ToString("N") + ".cmd")
    @"
@echo off
call "$env:VCVARS64" >nul
dumpbin /dependents "$Path"
"@ | Set-Content -LiteralPath $depsCmd -Encoding ASCII

    try {
        $output = & cmd /c $depsCmd
    } finally {
        Remove-IfExists @($depsCmd)
    }

    $names = @()
    foreach ($line in $output) {
        if ($line -match "^\s*([A-Za-z0-9_.+-]+\.(dll|drv))\s*$") {
            $names += $Matches[1]
        }
    }
    return $names | Sort-Object -Unique
}

function Get-MsvcPathDirs {
    if (!($env:VCVARS64 -and (Test-ExistingPath $env:VCVARS64))) {
        return @()
    }

    $cmdPath = Join-Path ([System.IO.Path]::GetTempPath()) ("mradm-vcvars-path-" + [System.Guid]::NewGuid().ToString("N") + ".cmd")
    @"
@echo off
call "$env:VCVARS64" >nul
echo __PATH__=%PATH%
echo __VCToolsRedistDir__=%VCToolsRedistDir%
echo __VCINSTALLDIR__=%VCINSTALLDIR%
"@ | Set-Content -LiteralPath $cmdPath -Encoding ASCII

    try {
        $output = & cmd /c $cmdPath
    } finally {
        Remove-IfExists @($cmdPath)
    }

    $dirs = @()
    foreach ($line in $output) {
        if ($line -like "__PATH__=*") {
            $dirs += ($line.Substring("__PATH__=".Length) -split [System.IO.Path]::PathSeparator)
        } elseif ($line -like "__VCToolsRedistDir__=*") {
            $redist = $line.Substring("__VCToolsRedistDir__=".Length)
            if (![string]::IsNullOrWhiteSpace($redist)) {
                $dirs += Join-Path $redist "x64\Microsoft.VC143.CRT"
            }
        } elseif ($line -like "__VCINSTALLDIR__=*") {
            $vcInstallDir = $line.Substring("__VCINSTALLDIR__=".Length)
            if (![string]::IsNullOrWhiteSpace($vcInstallDir)) {
                $redistRoot = Join-Path $vcInstallDir "Redist\MSVC"
                if (Test-ExistingPath $redistRoot) {
                    $dirs += Get-ChildItem -LiteralPath $redistRoot -Directory |
                        ForEach-Object { Join-Path $_.FullName "x64\Microsoft.VC143.CRT" }
                }
            }
        }
    }
    return $dirs |
        ForEach-Object { Resolve-ExistingPath $_ } |
        Where-Object { $_ } |
        Select-Object -Unique
}

function Find-Dll {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$SearchDirs
    )

    foreach ($dir in $SearchDirs) {
        if (!(Test-ExistingPath $dir)) {
            continue
        }
        $candidate = Join-Path $dir $Name
        if (Test-ExistingPath $candidate) {
            return Resolve-ExistingPath $candidate
        }
    }
    return $null
}

function Get-VersionMetadata {
    param([Parameter(Mandatory = $true)][string]$Field)

    $versionTool = Join-Path $repoRoot "scripts\release\version_metadata.py"
    if (Get-Command py -ErrorAction SilentlyContinue) {
        return (& py -3 $versionTool --repo-root $repoRoot --field $Field).Trim()
    }
    return (& python $versionTool --repo-root $repoRoot --field $Field).Trim()
}

$shortSha = (& git -C $repoRoot rev-parse --short=12 HEAD).Trim()
$commitSha = (& git -C $repoRoot rev-parse HEAD).Trim()
$productVersion = Get-VersionMetadata "product-version"
$cApiVersion = Get-VersionMetadata "c-api-version"
$version = Get-VersionMetadata "package-version"

$distDir = Join-Path $repoRoot "dist"
$packageName = "mradm-$version-windows-$Arch"
$packageRoot = Join-Path $distDir $packageName
$binDir = Join-Path $packageRoot "bin"
$archive = Join-Path $distDir "$packageName.zip"
$checksum = "$archive.sha256"

Write-Host "Preparing Windows CLI package: $packageName"
Remove-IfExists @($packageRoot, $archive, $checksum)
New-Item -ItemType Directory -Force -Path $binDir | Out-Null

Copy-Item -LiteralPath $binaryPath -Destination (Join-Path $binDir "mradm.exe") -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $packageRoot "LICENSE") -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "docs\THIRD_PARTY_LICENSES.md") -Destination (Join-Path $packageRoot "THIRD_PARTY_NOTICES.md") -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "third_party\licenses") -Destination (Join-Path $packageRoot "licenses") -Recurse -Force
Copy-Item -LiteralPath (Join-Path $repoRoot "third_party\sbom.cyclonedx.json") -Destination (Join-Path $packageRoot "sbom.cyclonedx.json") -Force

$dllSearchDirs = @()
if (Test-ExistingPath $OpenBlasRoot) {
    $dllSearchDirs += Join-Path $OpenBlasRoot "bin"
}
if (Test-ExistingPath $VcpkgRoot) {
    $dllSearchDirs += Join-Path $VcpkgRoot "installed\x64-windows\bin"
}
$dllSearchDirs += Split-Path -Parent $binaryPath
$dllSearchDirs += Get-MsvcPathDirs
if ($env:PATH) {
    $dllSearchDirs += $env:PATH -split [System.IO.Path]::PathSeparator
}
$dllSearchDirs = $dllSearchDirs |
    ForEach-Object { Resolve-ExistingPath $_ } |
    Where-Object { $_ } |
    Select-Object -Unique
Write-Host "DLL search directories: $($dllSearchDirs.Count)"

$queue = [System.Collections.Generic.Queue[string]]::new()
$seenBinaries = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$packagedDlls = [System.Collections.Generic.Dictionary[string, string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$missingDlls = [System.Collections.Generic.List[string]]::new()
$queue.Enqueue((Join-Path $binDir "mradm.exe"))

while ($queue.Count -gt 0) {
    $scanPath = $queue.Dequeue()
    $resolvedScanPath = Resolve-ExistingPath $scanPath
    if (!$resolvedScanPath) {
        throw "queued dependency path is missing: $scanPath"
    }
    if (!$seenBinaries.Add($resolvedScanPath)) {
        continue
    }

    Write-Host "Scanning dependencies: $resolvedScanPath"
    foreach ($dll in Get-DependentDllNames -Path $resolvedScanPath) {
        if (Test-SystemDll -Name $dll) {
            continue
        }

        $packagedPath = Join-Path $binDir $dll
        if (!(Test-ExistingPath $packagedPath)) {
            $source = Find-Dll -Name $dll -SearchDirs $dllSearchDirs
            if (!$source) {
                [void]$missingDlls.Add("$dll required by $resolvedScanPath")
                continue
            }
            Copy-Item -LiteralPath $source -Destination $packagedPath -Force
            $packagedDlls[$dll] = $source
        } elseif (!$packagedDlls.ContainsKey($dll)) {
            $packagedDlls[$dll] = $packagedPath
        }

        $queue.Enqueue($packagedPath)
    }
}

if ($missingDlls.Count -gt 0) {
    throw "Windows release package is missing non-system DLLs:`n$($missingDlls -join "`n")"
}

$depsFile = Join-Path $packageRoot "DEPENDENCIES.txt"
@("Windows dependency scan:", "") | Set-Content -LiteralPath $depsFile -Encoding utf8
foreach ($dependencyBinary in Get-ChildItem -LiteralPath $binDir -File | Where-Object { $_.Extension -in @(".exe", ".dll") } | Sort-Object Name) {
    "== bin\$($dependencyBinary.Name)" | Add-Content -LiteralPath $depsFile -Encoding utf8
    foreach ($dll in Get-DependentDllNames -Path $dependencyBinary.FullName) {
        $classification = if (Test-SystemDll -Name $dll) { "system" } elseif (Test-ExistingPath (Join-Path $binDir $dll)) { "packaged" } else { "missing" }
        "  $dll [$classification]" | Add-Content -LiteralPath $depsFile -Encoding utf8
        if ($classification -eq "missing") {
            throw "Windows release dependency escaped packaging: $dll required by $($dependencyBinary.FullName)"
        }
    }
    "" | Add-Content -LiteralPath $depsFile -Encoding utf8
}

"Packaged DLL sources:" | Add-Content -LiteralPath $depsFile -Encoding utf8
foreach ($entry in $packagedDlls.GetEnumerator() | Sort-Object Name) {
    "  $($entry.Key) <= $($entry.Value)" | Add-Content -LiteralPath $depsFile -Encoding utf8
}

$buildInfo = @(
    "name: MacinRender ADM Core",
    "binary: mradm.exe",
    "version: $version",
    "product_version: $productVersion",
    "c_api_version: $cApiVersion",
    "commit: $commitSha",
    "platform: windows",
    "arch: $Arch",
    "built_at_utc: $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))",
    "cmake_options: $(if ($CmakeOptions) { $CmakeOptions } else { 'not recorded' })",
    "dependency_policy: Windows packages may depend on Windows system DLLs only; every non-system DLL discovered by dumpbin /dependents must be copied into the package bin directory."
)
$buildInfo | Set-Content -LiteralPath (Join-Path $packageRoot "BUILD_INFO.txt") -Encoding utf8

Write-Host "Compressing package: $archive"
Compress-Archive -LiteralPath $packageRoot -DestinationPath $archive -Force
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash.ToLowerInvariant()
"$hash  $(Split-Path -Leaf $archive)" | Set-Content -LiteralPath $checksum -Encoding ASCII

Write-Host $archive
Write-Host $checksum
