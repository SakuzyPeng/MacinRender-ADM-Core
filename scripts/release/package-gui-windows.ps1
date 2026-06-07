param(
    [string]$Rid = "win-x64",
    [string]$OpenBlasRoot = "",
    [string]$VcpkgRoot = "",
    [string]$CmakeOptions = "",
    [switch]$SkipNative
)

$ErrorActionPreference = "Stop"

if ($Rid -ne "win-x64") {
    throw "unsupported GUI package RID '$Rid' (current script supports win-x64 only)"
}

if (![System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)) {
    throw "Windows GUI packaging must run on Windows"
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$project = Join-Path $repoRoot "gui\MacinRender.Gui\MacinRender.Gui.csproj"
$resolvedVcpkgRoot = if ($VcpkgRoot) { $VcpkgRoot } else { $env:VCPKG_INSTALLATION_ROOT }

function Quote-CmdArg {
    param([Parameter(Mandatory = $true)][string]$Value)
    return '"' + ($Value -replace '"', '\"') + '"'
}

function Invoke-VcvarsCommand {
    param([Parameter(Mandatory = $true)][string[]]$CommandLines)

    if (!($env:VCVARS64 -and (Test-Path $env:VCVARS64))) {
        throw "VCVARS64 must be set so Windows GUI packaging can use MSVC tools"
    }

    $cmdPath = Join-Path $env:TEMP ("mradm-gui-package-" + [System.Guid]::NewGuid().ToString("N") + ".cmd")
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("@echo off")
    $lines.Add("call " + (Quote-CmdArg $env:VCVARS64))
    $lines.Add("if errorlevel 1 exit /b %errorlevel%")
    foreach ($line in $CommandLines) {
        $lines.Add($line)
        $lines.Add("if errorlevel 1 exit /b %errorlevel%")
    }
    $lines | Set-Content -Path $cmdPath -Encoding ASCII

    try {
        & cmd /c $cmdPath
        if ($LASTEXITCODE -ne 0) {
            throw "MSVC command failed with exit code $LASTEXITCODE"
        }
    } finally {
        Remove-Item -Force $cmdPath -ErrorAction SilentlyContinue
    }
}

function Get-MsvcPathDirs {
    if (!($env:VCVARS64 -and (Test-Path $env:VCVARS64))) {
        return @()
    }

    $cmdPath = Join-Path $env:TEMP ("mradm-vcvars-path-" + [System.Guid]::NewGuid().ToString("N") + ".cmd")
    @"
@echo off
call "$env:VCVARS64" >nul
echo __PATH__=%PATH%
echo __VCToolsRedistDir__=%VCToolsRedistDir%
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
            $dirs += Join-Path $redist "x64\Microsoft.VC143.CRT"
        }
    }
    return $dirs | Where-Object { $_ -and (Test-Path $_) } | ForEach-Object { (Resolve-Path $_).Path } | Select-Object -Unique
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
        "dcomp.dll", "dnsapi.dll", "dwrite.dll", "dwmapi.dll", "dxgi.dll",
        "gdi32.dll", "gdi32full.dll", "gdiplus.dll", "hid.dll", "imm32.dll",
        "iphlpapi.dll", "kernel32.dll", "kernelbase.dll", "mf.dll", "mfplat.dll",
        "mfreadwrite.dll", "mpr.dll", "msvcp_win.dll", "msvcrt.dll", "ncrypt.dll",
        "netapi32.dll", "ntdll.dll", "ole32.dll", "oleacc.dll", "oleaut32.dll",
        "opengl32.dll", "powrprof.dll", "propsys.dll", "psapi.dll", "rpcrt4.dll",
        "sechost.dll", "setupapi.dll", "shcore.dll", "shell32.dll", "shlwapi.dll",
        "textinputframework.dll", "ucrtbase.dll", "user32.dll", "userenv.dll",
        "usp10.dll", "uxtheme.dll", "version.dll", "winhttp.dll", "wininet.dll",
        "winmm.dll", "winspool.drv", "wintypes.dll", "windowscodecs.dll",
        "wintrust.dll", "ws2_32.dll"
    ) | ForEach-Object { [void]$systemDlls.Add($_) }
    return $systemDlls.Contains($Name)
}

function Get-DependentDllNames {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (!($env:VCVARS64 -and (Test-Path $env:VCVARS64))) {
        throw "VCVARS64 must be set so dumpbin can scan Windows GUI release dependencies"
    }

    $depsCmd = Join-Path $env:TEMP ("mradm-gui-deps-" + [System.Guid]::NewGuid().ToString("N") + ".cmd")
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

function Find-Dll {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string[]]$SearchDirs
    )

    foreach ($dir in $SearchDirs) {
        if ([string]::IsNullOrWhiteSpace($dir) -or !(Test-Path $dir)) {
            continue
        }
        $candidate = Join-Path $dir $Name
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }
    return $null
}

if (!$SkipNative) {
    $nativeBuildDir = Join-Path $repoRoot "build\gui-native-package\$Rid"
    $runtimeNativeDir = Join-Path $repoRoot "gui\MacinRender.Gui\runtimes\$Rid\native"

    $cmakeArgs = @(
        "-S", $repoRoot,
        "-B", $nativeBuildDir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=MinSizeRel",
        "-DCMAKE_C_COMPILER=cl",
        "-DCMAKE_CXX_COMPILER=cl",
        "-DMR_ADM_BUILD_CAPI_BUNDLE=ON",
        "-DMR_ADM_CORE_BUILD_CLI=OFF",
        "-DMR_ADM_CORE_BUILD_TESTS=OFF",
        "-DMR_ADM_FLAC_PROVIDER=VENDORED",
        "-DMR_ADM_OPUS_PROVIDER=VENDORED",
        "-DMR_ADM_ENABLE_IAMF=OFF",
        "-DSAF_PERFORMANCE_LIB=SAF_USE_OPEN_BLAS_AND_LAPACKE"
    )

    $toolchainFile = ""
    if ($resolvedVcpkgRoot) {
        $candidate = Join-Path $resolvedVcpkgRoot "scripts\buildsystems\vcpkg.cmake"
        if (Test-Path $candidate) {
            $toolchainFile = (Resolve-Path $candidate).Path
        }
    }
    if ($toolchainFile) {
        $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile"
    }
    if ($env:FC_CACHE_DIR) {
        New-Item -ItemType Directory -Force -Path $env:FC_CACHE_DIR | Out-Null
        $cmakeArgs += "-DFETCHCONTENT_BASE_DIR=$env:FC_CACHE_DIR"
    }
    foreach ($varName in @("OPENBLAS_LIBRARY", "LAPACKE_LIBRARY", "OPENBLAS_HEADER_PATH", "LAPACKE_HEADER_PATH")) {
        $value = [Environment]::GetEnvironmentVariable($varName)
        if (![string]::IsNullOrWhiteSpace($value)) {
            $cmakeArgs += "-D$varName=$value"
        }
    }

    $configure = "cmake " + (($cmakeArgs | ForEach-Object { Quote-CmdArg $_ }) -join " ")
    $build = "cmake --build " + (Quote-CmdArg $nativeBuildDir) + " --target mradm_capi_bundle --config MinSizeRel"
    Invoke-VcvarsCommand -CommandLines @($configure, $build)

    $nativeDll = @(
        (Join-Path $nativeBuildDir "mradm_capi.dll"),
        (Join-Path $nativeBuildDir "MinSizeRel\mradm_capi.dll")
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (!$nativeDll) {
        $nativeDll = Get-ChildItem -Path $nativeBuildDir -Filter "mradm_capi.dll" -Recurse -File |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if (!$nativeDll) {
        throw "native C ABI bundle is missing under $nativeBuildDir"
    }

    New-Item -ItemType Directory -Force -Path $runtimeNativeDir | Out-Null
    Copy-Item $nativeDll -Destination (Join-Path $runtimeNativeDir "mradm_capi.dll") -Force
    Write-Host "copied native bundle: $(Join-Path $runtimeNativeDir 'mradm_capi.dll')"
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
$publishDir = Join-Path $repoRoot "build\gui-publish\$Rid"
$packageName = "MacinRender-Gui-$version-windows-x64"
$packageRoot = Join-Path $distDir $packageName
$appDir = Join-Path $packageRoot "app"
$archive = Join-Path $distDir "$packageName.zip"
$checksum = "$archive.sha256"

Remove-Item -Recurse -Force $publishDir, $packageRoot, $archive, $checksum -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $publishDir, $appDir | Out-Null

& dotnet publish $project -c Release -r $Rid --self-contained true -p:PublishAot=true -o $publishDir
if ($LASTEXITCODE -ne 0) {
    throw "dotnet publish failed with exit code $LASTEXITCODE"
}

Copy-Item (Join-Path $publishDir "*") -Destination $appDir -Recurse -Force
Copy-Item (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $packageRoot "LICENSE") -Force
Copy-Item (Join-Path $repoRoot "docs\THIRD_PARTY_LICENSES.md") -Destination (Join-Path $packageRoot "THIRD_PARTY_NOTICES.md") -Force

$exe = Join-Path $appDir "MacinRender.Gui.exe"
if (!(Test-Path $exe)) {
    throw "published GUI executable is missing: $exe"
}
if (!(Test-Path (Join-Path $appDir "mradm_capi.dll"))) {
    throw "published GUI native bundle is missing: $(Join-Path $appDir 'mradm_capi.dll')"
}

$dllSearchDirs = @()
if ($OpenBlasRoot -and (Test-Path $OpenBlasRoot)) {
    $dllSearchDirs += Join-Path $OpenBlasRoot "bin"
}
if ($resolvedVcpkgRoot -and (Test-Path $resolvedVcpkgRoot)) {
    $dllSearchDirs += Join-Path $resolvedVcpkgRoot "installed\x64-windows\bin"
}
$dllSearchDirs += $appDir
$dllSearchDirs += Get-MsvcPathDirs
if ($env:PATH) {
    $dllSearchDirs += $env:PATH -split [System.IO.Path]::PathSeparator
}
$dllSearchDirs = $dllSearchDirs | Where-Object { $_ -and (Test-Path $_) } | ForEach-Object { (Resolve-Path $_).Path } | Select-Object -Unique

$queue = [System.Collections.Generic.Queue[string]]::new()
$seenBinaries = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$packagedDlls = [System.Collections.Generic.Dictionary[string, string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$missingDlls = [System.Collections.Generic.List[string]]::new()
$queue.Enqueue($exe)
Get-ChildItem -Path $appDir -File | Where-Object { $_.Extension -eq ".dll" } | ForEach-Object { $queue.Enqueue($_.FullName) }

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

        $packagedPath = Join-Path $appDir $dll
        if (!(Test-Path $packagedPath)) {
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
    throw "Windows GUI release package is missing non-system DLLs:`n$($missingDlls -join "`n")"
}

$depsFile = Join-Path $packageRoot "DEPENDENCIES.txt"
@("Windows GUI dependency scan:", "") | Set-Content -Path $depsFile -Encoding utf8
foreach ($binary in Get-ChildItem -Path $appDir -File | Where-Object { $_.Extension -in @(".exe", ".dll") } | Sort-Object Name) {
    "== app\$($binary.Name)" | Add-Content -Path $depsFile -Encoding utf8
    foreach ($dll in Get-DependentDllNames -Path $binary.FullName) {
        $classification = if (Test-SystemDll -Name $dll) { "system" } elseif (Test-Path (Join-Path $appDir $dll)) { "packaged" } else { "missing" }
        "  $dll [$classification]" | Add-Content -Path $depsFile -Encoding utf8
        if ($classification -eq "missing") {
            throw "Windows GUI release dependency escaped packaging: $dll required by $($binary.FullName)"
        }
    }
    "" | Add-Content -Path $depsFile -Encoding utf8
}

"Packaged DLL sources:" | Add-Content -Path $depsFile -Encoding utf8
foreach ($entry in $packagedDlls.GetEnumerator() | Sort-Object Name) {
    "  $($entry.Key) <= $($entry.Value)" | Add-Content -Path $depsFile -Encoding utf8
}

$buildInfo = @(
    "name: MacinRender ADM GUI",
    "binary: app\MacinRender.Gui.exe",
    "version: $version",
    "commit: $commitSha",
    "rid: $Rid",
    "built_at_utc: $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))",
    "cmake_options: $(if ($CmakeOptions) { $CmakeOptions } else { 'not recorded' })",
    "dependency_policy: Windows GUI packages may depend on Windows system DLLs only; every non-system DLL discovered by dumpbin /dependents must be copied into the package app directory."
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
