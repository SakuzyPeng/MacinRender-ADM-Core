param(
    [Parameter(Mandatory = $true)]
    [string]$Archive
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $Archive)) {
    throw "package archive is missing: $Archive"
}

$checksum = "$Archive.sha256"
if (!(Test-Path $checksum)) {
    throw "package checksum is missing: $checksum"
}

$archivePath = (Resolve-Path $Archive).Path
$checksumPath = (Resolve-Path $checksum).Path
$expectedHash = ((Get-Content $checksumPath -Raw).Trim() -split "\s+")[0].ToLowerInvariant()
$actualHash = (Get-FileHash -Algorithm SHA256 -Path $archivePath).Hash.ToLowerInvariant()
if ($expectedHash -ne $actualHash) {
    throw "checksum mismatch: expected $expectedHash, got $actualHash"
}

$workDir = Join-Path ([System.IO.Path]::GetTempPath()) ("mradm-gui-win-smoke-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

try {
    Expand-Archive -Path $archivePath -DestinationPath $workDir -Force
    $roots = Get-ChildItem -Path $workDir -Directory
    if ($roots.Count -ne 1) {
        throw "package archive must contain exactly one top-level directory"
    }
    $packageRoot = $roots[0].FullName

    foreach ($required in @("app\MacinRender.Gui.exe", "app\mradm_capi.dll", "MacinRender ADM.cmd", "LICENSE", "THIRD_PARTY_NOTICES.md", "BUILD_INFO.txt", "DEPENDENCIES.txt", "licenses\INDEX.md", "sbom.cyclonedx.json")) {
        $path = Join-Path $packageRoot $required
        if (!(Test-Path $path)) {
            throw "package is missing $required"
        }
    }

    $launcher = Join-Path $packageRoot "MacinRender ADM.cmd"
    if ((Get-Content $launcher -Raw) -notmatch [regex]::Escape("app\MacinRender.Gui.exe")) {
        throw "package launcher does not point to app\MacinRender.Gui.exe"
    }

    $buildInfo = Get-Content (Join-Path $packageRoot "BUILD_INFO.txt") -Raw
    foreach ($field in @("version", "product_version", "c_api_version", "commit")) {
        if ($buildInfo -notmatch "(?m)^${field}: ") {
            throw "GUI package build info is missing $field"
        }
    }

    if ($buildInfo -match "(?m)^room_cuda: v2-trial") {
        foreach ($required in @("ROOM_CUDA.json", "ROOM_CUDA_README.txt", "MacinRender ADM CUDA Trial.cmd",
                "licenses\room-cuda\CUDA-EULA.txt", "licenses\room-cuda\CUDA-LICENSE.txt", "licenses\room-cuda\MathDx-LICENSE.txt")) {
            if (!(Test-Path -LiteralPath (Join-Path $packageRoot $required))) {
                throw "CUDA trial package is missing $required"
            }
        }
        $cuda = Get-Content -LiteralPath (Join-Path $packageRoot "ROOM_CUDA.json") -Raw | ConvertFrom-Json
        if ($cuda.profile -ne "room-cuda-v2" -or $cuda.native_optimizations -ne 22 -or
            $cuda.kernel_contract -ne 2 -or !$cuda.architectures.Count) {
            throw "CUDA trial package does not describe the retained V2 configuration"
        }
        $payload = @($cuda.files | ForEach-Object { $_.path })
        foreach ($pattern in @("mradm_room_cuda.dll", "cudart64*.dll", "cufft64*.dll", "nvrtc64*.dll", "nvrtc-builtins64*.dll")) {
            if (!($payload -like $pattern)) { throw "CUDA trial manifest is missing $pattern" }
        }
        foreach ($arch in $cuda.architectures) {
            $device = "room_cuda/target-$($cuda.target_source_sha256.Substring(0, 16))-sm$arch.ptx"
            if ($device -notin $payload) { throw "CUDA trial manifest is missing $device" }
        }
        foreach ($file in $cuda.files) {
            if ([System.IO.Path]::IsPathRooted($file.path) -or $file.path -match "(^|[/\\])\.\.([/\\]|$)") {
                throw "CUDA manifest contains a non-relative payload path"
            }
            $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $packageRoot ("app/" + $file.path))).Hash.ToLowerInvariant()
            if ($actual -ne $file.sha256) { throw "CUDA payload checksum mismatch: $($file.path)" }
        }
        $cudaLauncher = Get-Content -LiteralPath (Join-Path $packageRoot "MacinRender ADM CUDA Trial.cmd") -Raw
        if ($cudaLauncher -notmatch 'set "MR_ADM_ROOM_CUDA=1"' -or
            $cudaLauncher -notmatch [regex]::Escape("app\MacinRender.Gui.exe")) {
            throw "CUDA trial launcher does not explicitly select the packaged backend"
        }
    } elseif (Test-Path -LiteralPath (Join-Path $packageRoot "app\mradm_room_cuda.dll")) {
        throw "CPU package unexpectedly contains an unrecorded ROOM CUDA plugin"
    }

    $deps = Join-Path $packageRoot "DEPENDENCIES.txt"
    if (Select-String -Path $deps -Pattern "\[missing\]" -Quiet) {
        throw "package dependency manifest contains missing DLLs"
    }
    foreach ($external in @("avrt.dll", "nvcuda.dll")) {
        if (Test-Path -LiteralPath (Join-Path $packageRoot ("app/" + $external))) {
            throw "package must use the installed Windows/driver library: $external"
        }
    }

    $exe = Join-Path $packageRoot "app\MacinRender.Gui.exe"
    $appDir = Split-Path -Parent $exe
    $selftestStdout = Join-Path $workDir "gui-selftest.stdout.txt"
    $selftestStderr = Join-Path $workDir "gui-selftest.stderr.txt"
    $selftest = Start-Process `
        -FilePath $exe `
        -ArgumentList "--selftest" `
        -WorkingDirectory $appDir `
        -RedirectStandardOutput $selftestStdout `
        -RedirectStandardError $selftestStderr `
        -Wait `
        -PassThru
    if ($selftest.ExitCode -ne 0) {
        $out = if (Test-Path $selftestStdout) { Get-Content $selftestStdout -Raw } else { "" }
        $err = if (Test-Path $selftestStderr) { Get-Content $selftestStderr -Raw } else { "" }
        throw "GUI selftest failed with exit code $($selftest.ExitCode)`nstdout:`n$out`nstderr:`n$err"
    }

    $stdout = Join-Path $workDir "gui-start.stdout.txt"
    $stderr = Join-Path $workDir "gui-start.stderr.txt"
    $process = Start-Process `
        -FilePath $exe `
        -WorkingDirectory $appDir `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -PassThru

    if ($process.WaitForExit(8000)) {
        $out = if (Test-Path $stdout) { Get-Content $stdout -Raw } else { "" }
        $err = if (Test-Path $stderr) { Get-Content $stderr -Raw } else { "" }
        throw "GUI exited during startup smoke with code $($process.ExitCode)`nstdout:`n$out`nstderr:`n$err"
    }

    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    $process.WaitForExit()
} finally {
    Remove-Item -Recurse -Force $workDir -ErrorAction SilentlyContinue
}
