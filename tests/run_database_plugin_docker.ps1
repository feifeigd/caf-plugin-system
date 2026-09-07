#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BuildDir = "$PSScriptRoot/../out/build/windows-x64",
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Debug',
    [ValidateSet('Mongo', 'Redis')]
    [string]$Backend = 'Mongo',
    [ValidateSet('worker_pool', 'entity_store')]
    [string]$RedisTest = 'worker_pool',
    [string]$Image = ''
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$buildRoot = [IO.Path]::GetFullPath($BuildDir)
$allowedRoot = [IO.Path]::GetFullPath("$PSScriptRoot/../out/build")
if (-not $buildRoot.StartsWith($allowedRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "BuildDir must be a child of $allowedRoot"
}
$isMongo = $Backend -eq 'Mongo'
if (-not $Image) { $Image = if ($isMongo) { 'mongo:7' } else { 'redis:7-alpine' } }
$backendFolder = $Backend.ToLowerInvariant()
$testName = if ($isMongo) { 'mongo_entity_store' } else { "redis_$RedisTest" }
$needsEntity = $isMongo -or $RedisTest -eq 'entity_store'
$uriVariable = if ($isMongo) { 'MONGO_TEST_URI' } else { 'REDIS_TEST_URI' }
$ownershipLabel = "database-plugin-$backendFolder"
$docker = (Get-Command docker -ErrorAction Stop).Source
$sourceExe = "$buildRoot/tests/$Configuration/test_$testName.exe"
$sourceCore = "$buildRoot/src/core/$Configuration/caf_plugin_core.dll"
$sourcePlugin = "$buildRoot/plugins/$backendFolder/$Configuration/${backendFolder}_plugin.dll"
$sourceEntity = "$buildRoot/plugins/entity_store/$Configuration/entity_store_plugin.dll"
$dependencies = "$buildRoot/vcpkg_installed/x64-windows/bin"
if ($Configuration -eq 'Debug') { $dependencies = "$buildRoot/vcpkg_installed/x64-windows/debug/bin" }
$artifacts = @($sourceExe, $sourceCore, $sourcePlugin)
if ($needsEntity) { $artifacts += $sourceEntity }
foreach ($path in @($artifacts) + @($dependencies)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing build artifact: $path" }
}
function Invoke-Docker {
    param([string[]]$Arguments)
    $output = & $docker @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Docker failed: $($output -join [Environment]::NewLine)" }
    return ($output -join [Environment]::NewLine).Trim()
}

$mutex = [Threading.Mutex]::new($false, 'Local\caf-entity-store-docker-tests')
$ownsMutex = $false
$runId = [Guid]::NewGuid().ToString('N')
$containerName = "caf-db-$backendFolder-$($runId.Substring(0,12))"
$containerId = ''
$runtime = "$buildRoot/tests/${testName}_docker-$Configuration-$($runId.Substring(0,12))"
$oldUri = [Environment]::GetEnvironmentVariable($uriVariable, 'Process')
$oldPath = $env:PATH
$process = $null
$attemptedStart = $false
$started = [DateTime]::UtcNow
$imageId = ''
$passed = $false
try {
    try { $ownsMutex = $mutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
    if (-not $ownsMutex) { throw 'Another EntityStore Docker test runner is active.' }
    $null = Invoke-Docker @('version', '--format', '{{.Server.Version}}')
    # No implicit download or image upgrade.
    $imageId = Invoke-Docker @('image', 'inspect', '--format', '{{.Id}}', $Image)
    $null = New-Item -ItemType Directory -Path $runtime
    foreach ($path in $artifacts) {
        Copy-Item -LiteralPath $path -Destination $runtime
    }
    # Reuse installed runtime dependencies instead of duplicating every DLL.
    $env:PATH = $dependencies + [IO.Path]::PathSeparator + $oldPath
    $attemptedStart = $true
    $arguments = @('run', '--detach', '--name', $containerName,
        '--label', "caf.test=$ownershipLabel", '--label', "caf.test.run=$runId")
    if ($isMongo) {
        $arguments += @('--publish', '127.0.0.1::27017',
            '--tmpfs', '/data/db', '--tmpfs', '/data/configdb',
            $Image, '--replSet', 'rs0', '--bind_ip_all',
            '--setParameter', 'enableTestCommands=1', '--oplogSize', '64')
    } else {
        $arguments += @('--publish', '127.0.0.1::6379', '--tmpfs', '/data',
            $Image, 'redis-server', '--save', '', '--appendonly', 'no')
    }
    $runOutput = Invoke-Docker $arguments
    $containerId = ($runOutput -split '\r?\n')[-1].Trim()
    if ($containerId -notmatch '^[0-9a-f]{64}$') { throw 'Docker did not return a valid container ID.' }
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    $ready = $false
    do {
        if ($isMongo) {
            $null = & $docker exec $containerId mongosh --quiet --eval 'quit(db.adminCommand({ping:1}).ok ? 0 : 1)' 2>$null
        } else {
            $null = & $docker exec $containerId redis-cli ping 2>$null
        }
        if ($LASTEXITCODE -eq 0) { $ready = $true; break }
        Start-Sleep -Milliseconds 400
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $ready) { throw "$Backend did not become ready." }
    if ($isMongo) {
        $null = Invoke-Docker @('exec', $containerId, 'mongosh', '--quiet', '--eval',
            'const r=rs.initiate({_id:"rs0",members:[{_id:0,host:"127.0.0.1:27017"}]}); if(!r.ok) quit(1)')
        $ready = $false
        do {
            $null = & $docker exec $containerId mongosh --quiet --eval 'quit(db.hello().isWritablePrimary ? 0 : 1)' 2>$null
            if ($LASTEXITCODE -eq 0) { $ready = $true; break }
            Start-Sleep -Milliseconds 400
        } while ([DateTime]::UtcNow -lt $deadline)
        if (-not $ready) { throw 'MongoDB replica set did not elect a primary.' }
    }
    $portName = if ($isMongo) { '27017/tcp' } else { '6379/tcp' }
    $port = Invoke-Docker @('port', $containerId, $portName)
    if ($port -notmatch '^127\.0\.0\.1:(\d+)$') { throw "Unexpected port binding: $port" }
    $testUri = if ($isMongo) {
        "mongodb://127.0.0.1:$($Matches[1])/caf_entity_test?replicaSet=rs0&directConnection=true"
    } else {
        "redis://127.0.0.1:$($Matches[1])/0"
    }
    [Environment]::SetEnvironmentVariable($uriVariable, $testUri, 'Process')
    $testExe = "$runtime/test_$testName.exe"
    $testArguments = '"' + "$runtime/${backendFolder}_plugin.dll" + '"'
    if ($needsEntity) { $testArguments += ' "' + "$runtime/entity_store_plugin.dll" + '"' }
    $stdout = "$runtime/stdout.log"
    $stderr = "$runtime/stderr.log"
    $startInfo = @{
        FilePath = $testExe; ArgumentList = $testArguments; WorkingDirectory = $runtime
        WindowStyle = 'Hidden'; PassThru = $true
        RedirectStandardOutput = $stdout; RedirectStandardError = $stderr
    }
    $process = Start-Process @startInfo
    if (-not $process.WaitForExit(120000)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        throw 'Database plugin test exceeded 120 seconds; forced termination is a failure.'
    }
    $process.WaitForExit()
    $exitCode = $process.ExitCode
    $text = (Get-Content -LiteralPath $stdout -Raw) + (Get-Content -LiteralPath $stderr -Raw)
    Write-Output $text
    if ($exitCode -ne 0) { throw "Database plugin test exited with $exitCode." }
    $success = if ($isMongo) {
        $text -match 'Mongo EntityStore E2E passed' -and
        $text -match 'Mongo workers joined; graceful shutdown confirmed'
    } elseif ($RedisTest -eq 'entity_store') {
        $text -match 'Redis EntityStore E2E passed; workers joined and persistent replay confirmed'
    } else {
        $text -match 'Redis worker pool E2E passed; normal and forced exits joined workers'
    }
    if (-not $success) { throw "$Backend test did not confirm assertions and natural shutdown." }
    if ($text -match 'Detected memory leaks|Dumping objects ->|Assertion failed') {
        throw 'Database plugin test reported a CRT leak or assertion failure.'
    }
    $passed = $true
}
finally {
    [Environment]::SetEnvironmentVariable($uriVariable, $oldUri, 'Process')
    $env:PATH = $oldPath
    if ($process) {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
        $process.Dispose()
    }
    try {
        if ($attemptedStart) {
            # Resolve by this run's unique name, then verify both ownership labels.
            $inspectOutput = & $docker inspect $containerName 2>$null
            if ($LASTEXITCODE -eq 0) {
                $info = ($inspectOutput -join [Environment]::NewLine | ConvertFrom-Json)[0]
                if ($info.Name -ne "/$containerName" -or
                    $info.Config.Labels.'caf.test' -ne $ownershipLabel -or
                    $info.Config.Labels.'caf.test.run' -ne $runId -or
                    $info.Id -notmatch '^[0-9a-f]{64}$') {
                    throw 'Refusing to remove a container whose ownership was not verified.'
                }
                $containerId = $info.Id
                try {
                    & $docker logs $containerId 2>&1 | Set-Content -LiteralPath "$runtime/database.log"
                } finally {
                    # A full disk/logging failure must not strand the container.
                    $null = Invoke-Docker @('rm', '--force', '--volumes', $containerId)
                    $null = & $docker inspect $containerId 2>$null
                    if ($LASTEXITCODE -eq 0) { throw 'Temporary database plugin container was not removed.' }
                }
                Write-Output "Removed temporary database plugin container $containerId"
            } elseif ($containerId) {
                throw "Cannot verify cleanup of database plugin container $containerId; inspect Docker state."
            }
        }
    }
    finally {
        try {
            if (Test-Path -LiteralPath $runtime) {
                $resolvedRuntime = (Resolve-Path -LiteralPath $runtime).Path
                if ([IO.Path]::GetDirectoryName($resolvedRuntime) -ne
                    [IO.Path]::GetFullPath("$buildRoot/tests")) { throw 'Unexpected runtime cleanup target.' }
                Get-ChildItem -LiteralPath $resolvedRuntime -File |
                    Where-Object Extension -in @('.exe', '.dll', '.pdb') |
                    ForEach-Object { Remove-Item -LiteralPath $_.FullName }
                [ordered]@{
                    image = $Image; image_id = $imageId; container_id = $containerId
                    run_id = $runId; started_utc = $started.ToString('o')
                    finished_utc = [DateTime]::UtcNow.ToString('o'); passed = $passed
                } | ConvertTo-Json | Set-Content -LiteralPath "$runtime/docker-run.json"
                Write-Output "Database plugin test logs: $runtime"
            }
        } finally {
            if ($ownsMutex) { $mutex.ReleaseMutex() }
            $mutex.Dispose()
        }
    }
}
