#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BuildDir = "$PSScriptRoot/../out/build/windows-x64",
    [string]$Configuration = 'Debug',
    [string]$RedisImage = 'redis:7-alpine'
)
& "$PSScriptRoot/run_database_plugin_docker.ps1" -BuildDir $BuildDir -Configuration $Configuration -Backend Redis -RedisTest entity_store -Image $RedisImage
