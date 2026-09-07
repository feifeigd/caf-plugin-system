#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BuildDir = "$PSScriptRoot/../out/build/windows-x64",
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Debug',
    [string]$RedisImage = 'redis:7-alpine'
)
& "$PSScriptRoot/run_database_plugin_docker.ps1" -Backend Redis -Image $RedisImage -BuildDir $BuildDir -Configuration $Configuration
