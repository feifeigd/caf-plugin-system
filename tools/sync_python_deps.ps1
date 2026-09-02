param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"
$preset = "windows-x64-$($Configuration.ToLowerInvariant())"
cmake --build --preset $preset --target sync_python_deps
if ($LASTEXITCODE -ne 0) { throw "Python dependency sync failed ($LASTEXITCODE)" }
