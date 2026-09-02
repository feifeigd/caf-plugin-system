param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$projectDir = Join-Path $repoRoot "plugins/py_host/python"
$targetDir = Join-Path $repoRoot "run/$Configuration/Lib/site-packages"
$pythonExe = Join-Path $repoRoot `
    "out/build/windows-x64/vcpkg_installed/x64-windows/tools/python3/python.exe"
$requirements = Join-Path ([System.IO.Path]::GetTempPath()) "caf-py-host-$PID-requirements.txt"

if (-not (Test-Path -LiteralPath $pythonExe)) {
    throw "vcpkg Python not found: $pythonExe (configure/build the project first)"
}

try {
    uv export --project $projectDir --frozen --no-dev --no-emit-project `
        --format requirements-txt --output-file $requirements
    if ($LASTEXITCODE -ne 0) { throw "uv export failed ($LASTEXITCODE)" }
    uv pip sync --python $pythonExe --target $targetDir `
        --allow-empty-requirements $requirements
    if ($LASTEXITCODE -ne 0) { throw "uv pip sync failed ($LASTEXITCODE)" }
} finally {
    Remove-Item -LiteralPath $requirements -ErrorAction SilentlyContinue
}
