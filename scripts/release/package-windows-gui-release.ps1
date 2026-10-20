param(
    [string]$Rid = "win-x64",
    [string]$OpenBlasRoot = "",
    [string]$VcpkgRoot = "",
    [string]$CmakeOptions = "",
    [string]$NativeBuildDir = "",
    [switch]$RoomCuda,
    [string]$MathDxIncludeDir = "",
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
$nativeBuildDir = if ($NativeBuildDir) { [System.IO.Path]::GetFullPath($NativeBuildDir) } else { Join-Path $repoRoot "build\win-canon" }
$runtimeNativeDir = Join-Path $repoRoot "gui\MacinRender.Gui\runtimes\$Rid\native"

function Test-ExistingPath {
    param([AllowNull()][object]$Path)

    $text = [string]$Path
    if ([string]::IsNullOrWhiteSpace($text)) {
        return $false
    }
    return Test-Path -LiteralPath $text
}

function Quote-CmdArg {
    param([Parameter(Mandatory = $true)][string]$Value)
    return '"' + ($Value -replace '"', '\"') + '"'
}

function Invoke-VcvarsCommand {
    param([Parameter(Mandatory = $true)][string[]]$CommandLines)

    if (!($env:VCVARS64 -and (Test-ExistingPath $env:VCVARS64))) {
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

function Test-SystemDll {
    param([Parameter(Mandatory = $true)][string]$Name)

    $lower = $Name.ToLowerInvariant()
    if ($lower -like "api-ms-win-*.dll" -or $lower -like "ext-ms-win-*.dll") {
        return $true
    }

    $systemDlls = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    @(
        "advapi32.dll", "avrt.dll", "bcrypt.dll", "bcryptprimitives.dll", "cfgmgr32.dll",
        "combase.dll", "comctl32.dll", "comdlg32.dll", "coremessaging.dll",
        "crypt32.dll", "cryptbase.dll", "cryptsp.dll", "d2d1.dll", "d3d11.dll",
        "d3d12.dll", "d3dcompiler_47.dll", "dcomp.dll", "dnsapi.dll",
        "dwrite.dll", "dwmapi.dll", "dxgi.dll", "fontsub.dll", "gdi32.dll",
        "gdi32full.dll", "gdiplus.dll", "hid.dll", "imm32.dll",
        "iphlpapi.dll", "kernel32.dll", "kernelbase.dll", "mf.dll", "mfplat.dll",
        "mfreadwrite.dll", "mpr.dll", "msvcp_win.dll", "msvcrt.dll", "ncrypt.dll",
        "netapi32.dll", "ntdll.dll", "nvcuda.dll", "ole32.dll", "oleacc.dll", "oleaut32.dll",
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

    if (!($env:VCVARS64 -and (Test-ExistingPath $env:VCVARS64))) {
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

if (!$SkipNative) {
    $cmakeArgs = @(
        "-S", $repoRoot,
        "-B", $nativeBuildDir,
        "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=cl",
        "-DCMAKE_CXX_COMPILER=cl",
        "-DMR_ADM_BUILD_CAPI_BUNDLE=ON",
        "-DMR_ADM_FLAC_PROVIDER=VENDORED",
        "-DMR_ADM_OPUS_PROVIDER=VENDORED",
        "-DMR_ADM_ENABLE_IAMF=OFF",
        "-DMR_ADM_ENABLE_SOFA=ON",
        "-DMR_ADM_ENABLE_ROOM_CUDA=$(if ($RoomCuda) { 'ON' } else { 'OFF' })",
        "-DMR_ADM_BUILD_SAF_REFERENCE_TESTS=OFF"
    )
    if ($RoomCuda) {
        $cmakeArgs += @(
            "-DMR_ADM_ROOM_CUDA_FUSE_TARGET=ON",
            "-DMR_ADM_ROOM_CUDA_FUSE_CONVOLVE=OFF",
            "-DMR_ADM_ROOM_CUDA_COMPACT_UPLOAD=ON",
            "-DMR_ADM_ROOM_CUDA_FREQUENCY_MAJOR=ON"
        )
        if ($MathDxIncludeDir) {
            $cmakeArgs += "-DMR_ADM_ROOM_CUDA_MATHDX_INCLUDE_DIR=$MathDxIncludeDir"
        }
    }

    $toolchainFile = ""
    if ($resolvedVcpkgRoot) {
        $candidate = Join-Path $resolvedVcpkgRoot "scripts\buildsystems\vcpkg.cmake"
        if (Test-ExistingPath $candidate) {
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

    $configure = "cmake " + (($cmakeArgs | ForEach-Object { Quote-CmdArg $_ }) -join " ")
    $build = "cmake --build " + (Quote-CmdArg $nativeBuildDir) + " --target mradm_capi_bundle --config Release"
    Invoke-VcvarsCommand -CommandLines @($configure, $build)
}

# SkipNative skips compilation only. Always stage the selected canonical build,
# so a stale GUI runtime cannot silently select a different native variant.
$cache = @{}
foreach ($line in Get-Content (Join-Path $nativeBuildDir "CMakeCache.txt")) {
    if ($line -match "^([^#/:][^:]*):[^=]+=(.*)$") {
        $cache[$Matches[1]] = $Matches[2]
    }
}
if ($cache["CMAKE_BUILD_TYPE"] -ne "Release") {
    throw "GUI packaging requires a Release native build"
}
if ($cache["MR_ADM_ENABLE_SOFA"] -notin @("ON", "TRUE", "1")) {
    throw "GUI packaging requires MR_ADM_ENABLE_SOFA=ON; rebuild the native bundle with SOFA support"
}
$cudaEnabled = $cache["MR_ADM_ENABLE_ROOM_CUDA"] -in @("ON", "TRUE", "1")
if ($cudaEnabled -ne [bool]$RoomCuda) {
    throw "native ROOM CUDA configuration does not match -RoomCuda; rebuild the requested variant"
}
if ($RoomCuda) {
    foreach ($key in @("MR_ADM_ROOM_CUDA_FUSE_TARGET", "MR_ADM_ROOM_CUDA_COMPACT_UPLOAD", "MR_ADM_ROOM_CUDA_FREQUENCY_MAJOR")) {
        if ($cache[$key] -notin @("ON", "TRUE", "1")) { throw "V2 trial package requires $key=ON" }
    }
    if ($cache["MR_ADM_ROOM_CUDA_FUSE_CONVOLVE"] -notin @("OFF", "FALSE", "0")) {
        throw "V2 trial package requires MR_ADM_ROOM_CUDA_FUSE_CONVOLVE=OFF"
    }
}
$nativeDll = @(
    (Join-Path $nativeBuildDir "mradm_capi.dll"),
    (Join-Path $nativeBuildDir "Release\mradm_capi.dll")
) | Where-Object { Test-ExistingPath $_ } | Select-Object -First 1
if (!$nativeDll) { throw "native C ABI bundle is missing under $nativeBuildDir" }
& python (Join-Path $repoRoot "scripts\quality\check-capi-exports.py") $nativeDll
if ($LASTEXITCODE -ne 0) { throw "native C ABI bundle export surface does not match c_api.h" }

New-Item -ItemType Directory -Force -Path $runtimeNativeDir | Out-Null
Copy-Item $nativeDll -Destination (Join-Path $runtimeNativeDir "mradm_capi.dll") -Force
Write-Host "copied native bundle: $(Join-Path $runtimeNativeDir 'mradm_capi.dll')"

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
$workingTreeDirty = [bool](& git -C $repoRoot status --porcelain --untracked-files=normal)
$productVersion = Get-VersionMetadata "product-version"
$cApiVersion = Get-VersionMetadata "c-api-version"
$version = Get-VersionMetadata "package-version"

$distDir = Join-Path $repoRoot "dist"
$publishDir = Join-Path $repoRoot "build\gui-publish\$Rid"
$packageName = "MacinRender-Gui-$version-windows-x64"
if ($RoomCuda) { $packageName += "-cuda-trial" }
$packageRoot = Join-Path $distDir $packageName
$appDir = Join-Path $packageRoot "app"
$archive = Join-Path $distDir "$packageName.zip"
$checksum = "$archive.sha256"

Remove-Item -Recurse -Force $publishDir, $packageRoot, $archive, $checksum -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $publishDir, $appDir | Out-Null

& dotnet publish $project -c Release -r $Rid --self-contained true -p:PublishAot=true -p:DebugType=none -p:DebugSymbols=false `
    -p:Version=$productVersion -p:FileVersion=$productVersion -p:InformationalVersion="$version+$shortSha" -o $publishDir
if ($LASTEXITCODE -ne 0) {
    throw "dotnet publish failed with exit code $LASTEXITCODE"
}

Get-ChildItem -Path $publishDir -Recurse -Include "*.pdb" -File | Remove-Item -Force

Copy-Item (Join-Path $publishDir "*") -Destination $appDir -Recurse -Force
Copy-Item (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $packageRoot "LICENSE") -Force
Copy-Item (Join-Path $repoRoot "docs\THIRD_PARTY_LICENSES.md") -Destination (Join-Path $packageRoot "THIRD_PARTY_NOTICES.md") -Force
Copy-Item (Join-Path $repoRoot "third_party\licenses") -Destination (Join-Path $packageRoot "licenses") -Recurse -Force
Copy-Item (Join-Path $repoRoot "third_party\sbom.cyclonedx.json") -Destination (Join-Path $packageRoot "sbom.cyclonedx.json") -Force

$exe = Join-Path $appDir "MacinRender.Gui.exe"
if (!(Test-ExistingPath $exe)) {
    throw "published GUI executable is missing: $exe"
}
if (!(Test-ExistingPath (Join-Path $appDir "mradm_capi.dll"))) {
    throw "published GUI native bundle is missing: $(Join-Path $appDir 'mradm_capi.dll')"
}

$launcher = Join-Path $packageRoot "MacinRender ADM.cmd"
@(
    "@echo off",
    "start """" ""%~dp0app\MacinRender.Gui.exe"" %*"
) | Set-Content -Path $launcher -Encoding ASCII

if ($RoomCuda) {
    $nativeDir = Split-Path -Parent $nativeDll
    $plugin = Join-Path $nativeDir "mradm_room_cuda.dll"
    if (!(Test-ExistingPath $plugin)) { throw "optional ROOM CUDA plugin is missing" }
    Copy-Item $plugin -Destination $appDir -Force
    $cudaFiles = @("mradm_room_cuda.dll")
    foreach ($runtime in ($cache["MR_ADM_ROOM_CUDA_RUNTIME_FILES"] -split ";")) {
        if (!(Test-ExistingPath $runtime)) { throw "missing ROOM CUDA runtime: $runtime" }
        Copy-Item $runtime -Destination $appDir -Force
        $cudaFiles += Split-Path -Leaf $runtime
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $appDir "room_cuda") | Out-Null
    foreach ($device in ($cache["MR_ADM_ROOM_CUDA_DEVICE_FILES"] -split ";")) {
        if (!(Test-ExistingPath $device)) { throw "missing precompiled ROOM CUDA target FFT: $device" }
        Copy-Item $device -Destination (Join-Path $appDir "room_cuda") -Force
        $cudaFiles += "room_cuda/" + (Split-Path -Leaf $device)
    }
    $cudaLicenseDir = Join-Path $packageRoot "licenses\room-cuda"
    New-Item -ItemType Directory -Force -Path $cudaLicenseDir | Out-Null
    $cudaRoot = Split-Path -Parent $cache["CUDAToolkit_BIN_DIR"]
    Copy-Item (Join-Path $cudaRoot "EULA.txt") -Destination (Join-Path $cudaLicenseDir "CUDA-EULA.txt")
    Copy-Item (Join-Path $cudaRoot "LICENSE") -Destination (Join-Path $cudaLicenseDir "CUDA-LICENSE.txt")
    Copy-Item $cache["MR_ADM_ROOM_CUDA_MATHDX_LICENSE"] -Destination (Join-Path $cudaLicenseDir "MathDx-LICENSE.txt")
    $manifest = [ordered]@{
        profile = "room-cuda-v2"
        native_optimizations = 22
        kernel_contract = 2
        architectures = @($cache["MR_ADM_ROOM_CUDA_ARCHITECTURES"] -split ";")
        target_source_sha256 = $cache["MR_ADM_ROOM_CUDA_TARGET_SOURCE_SHA256"]
        files = @($cudaFiles | Sort-Object -Unique | ForEach-Object {
            [ordered]@{ path = $_; sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $appDir $_)).Hash.ToLowerInvariant() }
        })
    }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $packageRoot "ROOM_CUDA.json") -Encoding utf8
    @(
        "@echo off",
        "setlocal",
        'set "MR_ADM_ROOM_CUDA=1"',
        'start "" "%~dp0app\MacinRender.Gui.exe" %*'
    ) | Set-Content -Path (Join-Path $packageRoot "MacinRender ADM CUDA Trial.cmd") -Encoding ASCII
    @"
ROOM CUDA V2 trial

Use "MacinRender ADM CUDA Trial.cmd" to opt in for this launch.
The ordinary launcher uses CPU unless MR_ADM_ROOM_CUDA is set externally.
Select the ROOM renderer, a 96 kHz room/HRIR and realtime quality.
Exact quality always uses CPU. Other renderers are unaffected.
This package requires a compatible NVIDIA driver and one of the SM
architectures listed in ROOM_CUDA.json. No CUDA Toolkit or MathDx SDK is
needed on the target computer. Keep the entire app directory together.
Unsupported configurations and missing dependencies report an error;
they do not silently substitute a different processing backend.

This is an explicit trial package. Windows 5.333333 ms zero-overrun
acceptance has not passed. Frequent room switching has no zero-overrun
promise. Preserve normal device buffering when evaluating playback.

NVIDIA runtime and MathDx notices are in licenses/room-cuda.
The root SBOM describes the base application; ROOM_CUDA.json lists this
optional component's binary/device payload and SHA-256 hashes.
"@ | Set-Content -Path (Join-Path $packageRoot "ROOM_CUDA_README.txt") -Encoding utf8
}

$dllSearchDirs = @()
if (Test-ExistingPath $OpenBlasRoot) {
    $dllSearchDirs += Join-Path $OpenBlasRoot "bin"
}
if (Test-ExistingPath $resolvedVcpkgRoot) {
    $dllSearchDirs += Join-Path $resolvedVcpkgRoot "installed\x64-windows\bin"
}
$dllSearchDirs += $appDir
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
    throw "Windows GUI release package is missing non-system DLLs:`n$($missingDlls -join "`n")"
}

$depsFile = Join-Path $packageRoot "DEPENDENCIES.txt"
@("Windows GUI dependency scan:", "") | Set-Content -Path $depsFile -Encoding utf8
foreach ($binary in Get-ChildItem -Path $appDir -File | Where-Object { $_.Extension -in @(".exe", ".dll") } | Sort-Object Name) {
    "== app\$($binary.Name)" | Add-Content -Path $depsFile -Encoding utf8
    foreach ($dll in Get-DependentDllNames -Path $binary.FullName) {
        $classification = if (Test-SystemDll -Name $dll) { "system" } elseif (Test-ExistingPath (Join-Path $appDir $dll)) { "packaged" } else { "missing" }
        "  $dll [$classification]" | Add-Content -Path $depsFile -Encoding utf8
        if ($classification -eq "missing") {
            throw "Windows GUI release dependency escaped packaging: $dll required by $($binary.FullName)"
        }
    }
    "" | Add-Content -Path $depsFile -Encoding utf8
}

"Packaged DLL identities:" | Add-Content -Path $depsFile -Encoding utf8
foreach ($entry in $packagedDlls.GetEnumerator() | Sort-Object Name) {
    $identity = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $appDir $entry.Key)).Hash.ToLowerInvariant()
    "  $($entry.Key) [sha256: $identity]" | Add-Content -Path $depsFile -Encoding utf8
}

$buildInfo = @(
    "name: MacinRender ADM GUI",
    "binary: app\MacinRender.Gui.exe",
    "launcher: MacinRender ADM.cmd",
    "version: $version",
    "product_version: $productVersion",
    "c_api_version: $cApiVersion",
    "commit: $commitSha",
    "working_tree_dirty: $workingTreeDirty",
    "room_cuda: $(if ($RoomCuda) { 'v2-trial (explicit opt-in)' } else { 'off' })",
    "rid: $Rid",
    "built_at_utc: $((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))",
    "cmake_options: $(if ($CmakeOptions) { $CmakeOptions } else { 'not recorded' })",
    "dependency_policy: Windows system DLLs and the installed NVIDIA driver (optional CUDA trial) are external; all other dependencies must be packaged."
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
