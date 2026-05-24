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

$workDir = Join-Path ([System.IO.Path]::GetTempPath()) ("mradm-win-smoke-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

try {
    Expand-Archive -Path $archivePath -DestinationPath $workDir -Force
    $roots = Get-ChildItem -Path $workDir -Directory
    if ($roots.Count -ne 1) {
        throw "package archive must contain exactly one top-level directory"
    }
    $packageRoot = $roots[0].FullName

    foreach ($required in @("bin\mradm.exe", "LICENSE", "THIRD_PARTY_NOTICES.md", "BUILD_INFO.txt", "DEPENDENCIES.txt")) {
        $path = Join-Path $packageRoot $required
        if (!(Test-Path $path)) {
            throw "package is missing $required"
        }
    }

    & (Join-Path $packageRoot "bin\mradm.exe") --version
    & (Join-Path $packageRoot "bin\mradm.exe") backends
} finally {
    Remove-Item -Recurse -Force $workDir -ErrorAction SilentlyContinue
}
