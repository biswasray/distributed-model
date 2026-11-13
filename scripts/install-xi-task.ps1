<#
.SYNOPSIS
  Registers XI (xi.exe) as a Task Scheduler task that runs at startup as SYSTEM
  (architecture/02-node-agent.md §8). Run in an elevated PowerShell on each node.

.EXAMPLE
  .\install-xi-task.ps1 -Coordinator http://192.168.1.10:8100
#>
param(
    [Parameter(Mandatory)][string]$Coordinator,
    [string]$Root = 'C:\qwen-cluster',
    [string]$TaskName = 'QwenNodeAgent'
)
$ErrorActionPreference = 'Stop'

$xi = Join-Path $Root 'bin\xi.exe'
if (-not (Test-Path $xi)) { throw "xi.exe not found at $xi" }
if (-not [Environment]::GetEnvironmentVariable('QWEN_CLUSTER_TOKEN', 'Machine')) {
    Write-Warning 'QWEN_CLUSTER_TOKEN is not set as a machine environment variable. The control plane will be unauthenticated.'
}

$argLine = "--coordinator $Coordinator --llama-dir `"$Root\llama.cpp`" --models-dir `"$Root\models`" --log-dir `"$Root\logs`""
$action = New-ScheduledTaskAction -Execute $xi -Argument $argLine -WorkingDirectory $Root
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings `
    -RunLevel Highest -User 'SYSTEM' -Force | Out-Null
Write-Host "Registered task '$TaskName': $xi $argLine"
Write-Host "Start it now with: Start-ScheduledTask -TaskName $TaskName"
