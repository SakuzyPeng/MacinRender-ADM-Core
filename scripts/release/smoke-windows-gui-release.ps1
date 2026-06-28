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

    foreach ($required in @("app\MacinRender.Gui.exe", "app\mradm_capi.dll", "LICENSE", "THIRD_PARTY_NOTICES.md", "BUILD_INFO.txt", "DEPENDENCIES.txt", "licenses\INDEX.md", "sbom.cyclonedx.json")) {
        $path = Join-Path $packageRoot $required
        if (!(Test-Path $path)) {
            throw "package is missing $required"
        }
    }

    $deps = Join-Path $packageRoot "DEPENDENCIES.txt"
    if (Select-String -Path $deps -Pattern "\[missing\]" -Quiet) {
        throw "package dependency manifest contains missing DLLs"
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
