<#
.SYNOPSIS
  Registers XI (xi.exe) as a Task Scheduler task that runs at startup as SYSTEM
  (architecture/02-node-agent.md §8). Run in an elevated PowerShell on each node.

  The agent reads its settings from <Root>\xi.yaml (created here if missing), so changes made on
  the agent's web page (http://<node>:8200/ui/) survive restarts. The token is not stored there:
  set QWEN_CLUSTER_TOKEN as a machine environment variable.

.EXAMPLE
  .\install-xi-task.ps1 -Coordinator http://192.168.1.10:8100

.EXAMPLE
  # No coordinator yet: the agent starts and waits; set the URL on http://127.0.0.1:8200/ui/.
  .\install-xi-task.ps1

.EXAMPLE
  # Also let the XGI web console run code on this node (ADR-009). Programs run as SYSTEM.
  .\install-xi-task.ps1 -Coordinator http://192.168.1.10:8100 -AllowExec
#>
param(
    [string]$Coordinator = '',
    [string]$Root = 'C:\qwen-cluster',
    [string]$TaskName = 'QwenNodeAgent',
    [switch]$AllowExec
)
$ErrorActionPreference = 'Stop'

$xi = Join-Path $Root 'bin\xi.exe'
if (-not (Test-Path $xi)) { throw "xi.exe not found at $xi" }
if (-not [Environment]::GetEnvironmentVariable('QWEN_CLUSTER_TOKEN', 'Machine')) {
    Write-Warning 'QWEN_CLUSTER_TOKEN is not set as a machine environment variable. The control plane will be unauthenticated.'
}

# Single-quoted YAML strings keep Windows backslashes literal.
function YamlQuote([string]$s) { "'" + $s.Replace("'", "''") + "'" }

$settingsFile = Join-Path $Root 'xi.yaml'
if (-not (Test-Path $settingsFile)) {
    @(
        '# XI node agent settings (architecture/02-node-agent.md section 10). Also editable on the'
        '# agent web page. The cluster token is NOT stored here (use QWEN_CLUSTER_TOKEN).'
        "coordinator: $(YamlQuote $Coordinator)"
        "llama_dir: $(YamlQuote "$Root\llama.cpp")"
        "models_dir: $(YamlQuote "$Root\models")"
        "log_dir: $(YamlQuote "$Root\logs")"
        "exec_dir: $(YamlQuote "$Root\exec")"
    ) | Set-Content -Path $settingsFile -Encoding ascii
    Write-Host "Created $settingsFile"
} elseif ($Coordinator) {
    $lines = @(Get-Content $settingsFile)
    if ($lines -match '^coordinator:') { $lines = $lines -replace '^coordinator:.*$', "coordinator: $(YamlQuote $Coordinator)" }
    else { $lines += "coordinator: $(YamlQuote $Coordinator)" }
    $lines | Set-Content -Path $settingsFile -Encoding ascii
    Write-Host "Set coordinator in $settingsFile (other settings kept)"
} else {
    Write-Host "Keeping $settingsFile"
}

$argLine = "--config `"$settingsFile`""
if ($AllowExec) {
    Write-Warning 'Console code execution is ON: anyone with the cluster token can run programs on this PC as SYSTEM.'
    $argLine += ' --allow-exec'
}
$action = New-ScheduledTaskAction -Execute $xi -Argument $argLine -WorkingDirectory $Root
$trigger = New-ScheduledTaskTrigger -AtStartup
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings `
    -RunLevel Highest -User 'SYSTEM' -Force | Out-Null
Write-Host "Registered task '$TaskName': $xi $argLine"
Write-Host "Start it now with: Start-ScheduledTask -TaskName $TaskName"
Write-Host "Agent page: http://127.0.0.1:8200/ui/"
