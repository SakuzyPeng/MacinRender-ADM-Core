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

function Test-ExistingPath {
    param([AllowNull()][object]$Path)

    $text = [string]$Path
    if ([string]::IsNullOrWhiteSpace($text)) {
        return $false
    }
    return Test-Path -LiteralPath $text
}

$binaryPath = if ([System.IO.Path]::IsPathRooted($Binary)) { $Binary } else { Join-Path $repoRoot $Binary }
if (!(Test-ExistingPath $binaryPath)) {
    throw "binary is missing: $binaryPath"
}

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
        "dbghelp.dll", "dcomp.dll", "dnsapi.dll", "dwrite.dll", "dwmapi.dll",
        "dxgi.dll", "gdi32.dll", "gdi32full.dll", "gdiplus.dll", "hid.dll",
        "imm32.dll", "iphlpapi.dll", "kernel32.dll", "kernelbase.dll", "mf.dll",
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

    if (!($env:VCVARS64 -and (Test-ExistingPath $env:VCVARS64))) {
        throw "VCVARS64 must be set so dumpbin can scan Windows release dependencies"
    }

    $depsCmd = Join-Path $env:TEMP ("mradm-release-deps-" + [System.Guid]::NewGuid().ToString("N") + ".cmd")
    @"
@echo off
call "$env:VCVARS64" >nul
dumpbin /dependents "$Path"
"@ | Set-Content -Path $depsCmd -Encoding ASCII

    try {
        $output = & cmd /c $depsCmd
    } finally {
        Remove-Item -Force $depsCmd -ErrorAction SilentlyContinue
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

    $cmdPath = Join-Path $env:TEMP ("mradm-vcvars-path-" + [System.Guid]::NewGuid().ToString("N") + ".cmd")
    @"
@echo off
call "$env:VCVARS64" >nul
echo __PATH__=%PATH%
echo __VCToolsRedistDir__=%VCToolsRedistDir%
echo __VCINSTALLDIR__=%VCINSTALLDIR%
"@ | Set-Content -Path $cmdPath -Encoding ASCII

    try {
        $output = & cmd /c $cmdPath
    } finally {
        Remove-Item -Force $cmdPath -ErrorAction SilentlyContinue
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
                    $dirs += Get-ChildItem -Path $redistRoot -Directory |
                        ForEach-Object { Join-Path $_.FullName "x64\Microsoft.VC143.CRT" }
                }
            }
        }
    }
    return $dirs |
        Where-Object { Test-ExistingPath $_ } |
        ForEach-Object { (Resolve-Path ([string]$_)).Path } |
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
            return (Resolve-Path $candidate).Path
        }
    }
    return $null
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
    Where-Object { Test-ExistingPath $_ } |
    ForEach-Object { (Resolve-Path ([string]$_)).Path } |
    Select-Object -Unique

$queue = [System.Collections.Generic.Queue[string]]::new()
$seenBinaries = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$packagedDlls = [System.Collections.Generic.Dictionary[string, string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$missingDlls = [System.Collections.Generic.List[string]]::new()
$queue.Enqueue((Join-Path $binDir "mradm.exe"))

while ($queue.Count -gt 0) {
    $scanPath = $queue.Dequeue()
    $resolvedScanPath = (Resolve-Path $scanPath).Path
    if (!$seenBinaries.Add($resolvedScanPath)) {
        continue
    }

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
            Copy-Item $source -Destination $packagedPath -Force
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
@("Windows dependency scan:", "") | Set-Content -Path $depsFile -Encoding utf8
foreach ($binary in Get-ChildItem -Path $binDir -File | Where-Object { $_.Extension -in @(".exe", ".dll") } | Sort-Object Name) {
    "== bin\$($binary.Name)" | Add-Content -Path $depsFile -Encoding utf8
    foreach ($dll in Get-DependentDllNames -Path $binary.FullName) {
        $classification = if (Test-SystemDll -Name $dll) { "system" } elseif (Test-ExistingPath (Join-Path $binDir $dll)) { "packaged" } else { "missing" }
        "  $dll [$classification]" | Add-Content -Path $depsFile -Encoding utf8
        if ($classification -eq "missing") {
            throw "Windows release dependency escaped packaging: $dll required by $($binary.FullName)"
        }
    }
    "" | Add-Content -Path $depsFile -Encoding utf8
}

"Packaged DLL sources:" | Add-Content -Path $depsFile -Encoding utf8
foreach ($entry in $packagedDlls.GetEnumerator() | Sort-Object Name) {
    "  $($entry.Key) <= $($entry.Value)" | Add-Content -Path $depsFile -Encoding utf8
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
    "dependency_policy: Windows packages may depend on Windows system DLLs only; every non-system DLL discovered by dumpbin /dependents must be copied into the package bin directory."
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
