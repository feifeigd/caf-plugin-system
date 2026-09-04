#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('all', 'mysql', 'postgres')]
    [string]$Backend = 'all',
    [string]$BuildDir = "$PSScriptRoot/../out/build/windows-x64",
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Debug',
    [string]$MySqlImage = 'mysql:8.0',
    [string]$PostgresImage = 'postgres:16-alpine',
    [ValidateRange(10, 300)]
    [int]$ReadyTimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$buildRoot = [IO.Path]::GetFullPath($BuildDir)
$allowedRoot = [IO.Path]::GetFullPath("$PSScriptRoot/../out/build")
if (-not $buildRoot.StartsWith($allowedRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "BuildDir must be a child of $allowedRoot"
}
$docker = (Get-Command docker -ErrorAction Stop).Source
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$app = "$buildRoot/src/app/$Configuration/caf_plugin_app.exe"
$core = "$buildRoot/src/core/$Configuration/caf_plugin_core.dll"
$entityStore = "$buildRoot/plugins/entity_store/$Configuration/entity_store_plugin.dll"
$mysqlCancellation = "$buildRoot/tests/$Configuration/test_mysql_cancellation.exe"
$thirdParty = "$buildRoot/vcpkg_installed/x64-windows/bin"
if ($Configuration -eq 'Debug') {
    $thirdParty = "$buildRoot/vcpkg_installed/x64-windows/debug/bin"
}
$backends = if ($Backend -eq 'all') { @('mysql', 'postgres') } else { @($Backend) }
if ($backends -contains 'mysql' -and -not (Test-Path -LiteralPath $mysqlCancellation)) {
    throw "Missing build artifact: $mysqlCancellation"
}
foreach ($path in @($app, $core, $entityStore, $thirdParty)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing build artifact: $path" }
}
foreach ($kind in $backends) {
    $plugin = "$buildRoot/plugins/$kind/$Configuration/${kind}_plugin.dll"
    if (-not (Test-Path -LiteralPath $plugin)) { throw "Missing build artifact: $plugin" }
}

function Invoke-DockerChecked {
    param([string[]]$Arguments)
    $output = & $docker @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Docker failed: $($output -join [Environment]::NewLine)" }
    return ($output -join [Environment]::NewLine).Trim()
}

# Serialize this runner across terminals; never stop unrelated database containers.
$mutex = [Threading.Mutex]::new($false, 'Local\caf-entity-store-docker-tests')
$ownsMutex = $false
$oldEnvironment = @{}
$envNames = @('MYSQL_DATABASE', 'MYSQL_USER', 'MYSQL_PASSWORD', 'MYSQL_ROOT_PASSWORD',
    'MYSQL_ROOT_HOST', 'POSTGRES_USER', 'POSTGRES_PASSWORD', 'POSTGRES_DB',
    'CAF_ENTITY_TEST_DB_URI', 'CAF_ENTITY_TEST_ADMIN_URI',
    'MYSQL_TEST_HOST', 'MYSQL_TEST_PORT', 'MYSQL_TEST_USER', 'MYSQL_TEST_PASSWORD', 'MYSQL_TEST_DATABASE')
try {
    try { $ownsMutex = $mutex.WaitOne(0) }
    catch [Threading.AbandonedMutexException] { $ownsMutex = $true }
    if (-not $ownsMutex) { throw 'Another EntityStore Docker test runner is active.' }
    foreach ($name in $envNames) {
        $oldEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    $null = Invoke-DockerChecked @('version', '--format', '{{.Server.Version}}')
    $failures = [Collections.Generic.List[string]]::new()
    foreach ($kind in $backends) {
        $runId = [Guid]::NewGuid().ToString('N')
        $containerName = "caf-entity-store-$kind-$($runId.Substring(0, 12))"
        $containerId = $null
        $runtime = "$buildRoot/tests/entity_store_docker_${kind}-$Configuration-$($runId.Substring(0, 12))"
        $testOutput = @()
        $imageId = ''
        $started = [DateTime]::UtcNow
        $password = [Guid]::NewGuid().ToString('N')
        $image = if ($kind -eq 'mysql') { $MySqlImage } else { $PostgresImage }
        try {
            $null = New-Item -ItemType Directory -Path $runtime -Force
            $imageId = Invoke-DockerChecked @('image', 'inspect', '--format', '{{.Id}}', $image)
            $runArgs = @('run', '--detach', '--rm', '--name', $containerName,
                '--label', 'caf.test=entity-store', '--label', "caf.test.run=$runId")
            if ($kind -eq 'mysql') {
                $env:MYSQL_DATABASE = 'entity_store_test'
                $env:MYSQL_USER = 'entity_test'
                $env:MYSQL_PASSWORD = $password
                $env:MYSQL_ROOT_PASSWORD = [Guid]::NewGuid().ToString('N')
                $env:MYSQL_ROOT_HOST = '%'
                $port = '3306'
                $runArgs += @('--publish', '127.0.0.1::3306', '--tmpfs', '/var/lib/mysql',
                    '--env', 'MYSQL_DATABASE', '--env', 'MYSQL_USER',
                    '--env', 'MYSQL_PASSWORD', '--env', 'MYSQL_ROOT_PASSWORD',
                    '--env', 'MYSQL_ROOT_HOST', $image,
                    '--character-set-server=utf8mb4', '--collation-server=utf8mb4_bin')
                $probe = 'MYSQL_PWD="$MYSQL_PASSWORD" mysql --connect-timeout=3 --protocol=tcp -h127.0.0.1 -u"$MYSQL_USER" "$MYSQL_DATABASE" -e "SELECT 1"'
            } else {
                $env:POSTGRES_USER = 'entity_test'
                $env:POSTGRES_PASSWORD = $password
                $env:POSTGRES_DB = 'entity_store_test'
                $port = '5432'
                $runArgs += @('--publish', '127.0.0.1::5432', '--tmpfs', '/var/lib/postgresql/data',
                    '--env', 'POSTGRES_USER', '--env', 'POSTGRES_PASSWORD',
                    '--env', 'POSTGRES_DB', $image)
                $probe = 'PGCONNECT_TIMEOUT=3 PGPASSWORD="$POSTGRES_PASSWORD" psql -h127.0.0.1 -U"$POSTGRES_USER" -d"$POSTGRES_DB" -w -c "SELECT 1"'
            }
            Write-Host "[$kind] Starting isolated container $containerName ($image)"
            $containerId = Invoke-DockerChecked $runArgs
            $deadline = [DateTime]::UtcNow.AddSeconds($ReadyTimeoutSeconds)
            $ready = $false
            do {
                $null = & $docker exec $containerId sh -c $probe 2>&1
                if ($LASTEXITCODE -eq 0) { $ready = $true; break }
                $running = Invoke-DockerChecked @('inspect', '--format', '{{.State.Running}}', $containerId)
                if ($running -ne 'true') { throw "$kind container exited during startup" }
                Start-Sleep -Seconds 1
            } while ([DateTime]::UtcNow -lt $deadline)
            if (-not $ready) { throw "$kind did not become ready within $ReadyTimeoutSeconds seconds" }
            $binding = Invoke-DockerChecked @('port', $containerId, "$port/tcp")
            if ($binding -notmatch '^127\.0\.0\.1:(\d+)$') { throw "Unexpected Docker port binding: $binding" }
            $hostPort = $Matches[1]
            if ($kind -eq 'mysql') {
                $env:MYSQL_TEST_HOST = '127.0.0.1'
                $env:MYSQL_TEST_PORT = $hostPort
                $env:MYSQL_TEST_USER = $env:MYSQL_USER
                $env:MYSQL_TEST_PASSWORD = $password
                $env:MYSQL_TEST_DATABASE = $env:MYSQL_DATABASE
            }
            $env:CAF_ENTITY_TEST_DB_URI = "${kind}://entity_test:${password}@127.0.0.1:${hostPort}/entity_store_test"
            $env:CAF_ENTITY_TEST_ADMIN_URI = if ($kind -eq 'mysql') {
                "mysql://root:$($env:MYSQL_ROOT_PASSWORD)@127.0.0.1:${hostPort}/entity_store_test"
            } else { $env:CAF_ENTITY_TEST_DB_URI }
            Write-Host "[$kind] Ready on loopback port $hostPort; running EntityStore integration checks"
            $testArgs = @("-DTEST_NAME=entity_store_docker_$kind", "-DBACKEND=$kind",
                "-DRUNTIME_DIR=$runtime", "-DAPP_EXE=$app", "-DCORE_DLL=$core",
                "-DBACKEND_DLL=$buildRoot/plugins/$kind/$Configuration/${kind}_plugin.dll",
                "-DMYSQL_CANCELLATION_EXE=$mysqlCancellation",
                "-DENTITY_STORE_DLL=$entityStore", "-DTHIRD_PARTY_DLL_DIR=$thirdParty",
                '-P', "$PSScriptRoot/run_entity_store_plugin_test.cmake")
            $testOutput = & $cmake @testArgs 2>&1
            $testExitCode = $LASTEXITCODE
            $testOutput | ForEach-Object { Write-Host $_ }
            if ($testExitCode -ne 0) { throw "$kind integration checks failed (exit $testExitCode)" }
            Get-Content -LiteralPath "$runtime/manual-stdout.log", "$runtime/database-stdout.log" |
                Select-String '\[EntityStoreTest\]|workers joined|framework shutdown complete' |
                ForEach-Object { Write-Host $_.Line }
            if ($kind -eq 'mysql') {
                Get-Content -LiteralPath "$runtime/cancellation-stdout.log" |
                    ForEach-Object { Write-Host $_ }
            }
        } catch {
            $failures.Add("${kind}: $($_.Exception.Message)")
            Write-Warning $failures[$failures.Count - 1]
        } finally {
            try {
                if (-not $containerId) {
                    # docker run may create a container without returning its ID.
                    $containerId = Invoke-DockerChecked @('ps', '--all', '--quiet',
                        '--filter', "name=^/$containerName$", '--filter', "label=caf.test.run=$runId")
                }
                try {
                    $null = New-Item -ItemType Directory -Path $runtime -Force
                    $testOutput | Out-File -LiteralPath "$runtime/test-runner.log" -Encoding utf8
                    if ($containerId) {
                        & $docker logs $containerId 2>&1 | Out-File -LiteralPath "$runtime/database.log" -Encoding utf8
                    }
                } finally {
                    # Cleanup must still run if collecting logs fails (e.g. full disk).
                    if ($containerId) {
                        $null = & $docker stop --time 10 $containerId 2>&1
                        $remaining = Invoke-DockerChecked @('ps', '--all', '--quiet', '--filter', "id=$containerId")
                        if ($remaining) {
                            $null = Invoke-DockerChecked @('rm', '--force', '--volumes', $containerId)
                            $remaining = Invoke-DockerChecked @('ps', '--all', '--quiet', '--filter', "id=$containerId")
                            if ($remaining) { throw "Container cleanup failed; refusing to start another database: $containerId" }
                        }
                        Write-Host "[$kind] Container removed; temporary database discarded"
                    }
                }
            } finally {
                # Also covers staging/cleanup failures before CMake removes credentials.
                $configPath = "$runtime/caf-application.conf"
                if (Test-Path -LiteralPath $configPath) { Remove-Item -LiteralPath $configPath }
            }
            [ordered]@{
                backend = $kind; image = $image; image_id = $imageId
                container_name = $containerName; container_id = $containerId
                started_utc = $started.ToString('o'); finished_utc = [DateTime]::UtcNow.ToString('o')
                container_removed = $true
            } | ConvertTo-Json | Out-File -LiteralPath "$runtime/docker-run.json" -Encoding utf8
            Write-Host "[$kind] Logs: $runtime"
        }
    }
    if ($failures.Count -gt 0) { throw ($failures -join [Environment]::NewLine) }
    Write-Host 'PASS: requested Docker databases tested sequentially; no test containers retained.'
} finally {
    foreach ($name in $oldEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $oldEnvironment[$name], 'Process')
    }
    if ($ownsMutex) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
