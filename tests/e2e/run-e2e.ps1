<#
.SYNOPSIS
  End-to-end test: xgi serve + 4 xi agents on localhost, driving mock llama.cpp binaries.

  Covers: registration, auth (cluster token + API keys), deploy/409, group start sequence,
  routing (stream + non-stream), unknown model, worker failure -> repair onto the spare,
  drain -> unplaced -> undrain -> healthy, restart, undeploy.
#>
param(
    [Parameter(Mandatory)][string]$Xgi,
    [Parameter(Mandatory)][string]$Xi,
    [Parameter(Mandatory)][string]$MockLlama,
    [Parameter(Mandatory)][string]$MockRpc,
    [string]$WorkDir = (Join-Path $env:TEMP "xgi-e2e-$PID"),
    [switch]$KeepWorkDir
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Net.Http

$token = 'e2e-token'
$apiKey = 'e2e-key'
$coord = 'http://127.0.0.1:18100'
$router = 'http://127.0.0.1:18000'
$procs = New-Object System.Collections.ArrayList
$script:failures = 0
$http = New-Object System.Net.Http.HttpClient
$http.Timeout = [TimeSpan]::FromSeconds(30)

function Log($msg) { Write-Host ("[{0:HH:mm:ss}] {1}" -f (Get-Date), $msg) }
function Check($cond, $what) {
    if ($cond) { Write-Host "  PASS  $what" -ForegroundColor Green }
    else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:failures++ }
}
function Call([string]$method, [string]$url, $body = $null, [hashtable]$headers = @{}) {
    $req = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::new($method), $url)
    foreach ($k in $headers.Keys) { [void]$req.Headers.TryAddWithoutValidation($k, $headers[$k]) }
    if ($null -ne $body) {
        $json = if ($body -is [string]) { $body } else { $body | ConvertTo-Json -Depth 10 -Compress }
        $req.Content = New-Object System.Net.Http.StringContent($json, [Text.Encoding]::UTF8, 'application/json')
    }
    try {
        $res = $http.SendAsync($req).GetAwaiter().GetResult()
        $text = $res.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        $parsed = $null
        try { $parsed = $text | ConvertFrom-Json } catch { }
        $hdr = @{}
        foreach ($h in $res.Headers) { $hdr[$h.Key] = ($h.Value -join ',') }
        return [pscustomobject]@{ Status = [int]$res.StatusCode; Text = $text; Json = $parsed; Headers = $hdr }
    } catch {
        return [pscustomobject]@{ Status = 0; Text = $_.Exception.Message; Json = $null; Headers = @{} }
    }
}
$T = @{ 'X-Cluster-Token' = $token }
$K = @{ 'Authorization' = "Bearer $apiKey" }
# Windows PowerShell 5.1 emits a JSON array as ONE pipeline object; foreach unrolls it.
function Items($x) { foreach ($i in $x) { $i } }
function Groups { Items (Call GET "$coord/groups" $null $T).Json }
function Nodes { Items (Call GET "$coord/nodes" $null $T).Json }
function Get-ClusterGroup($id) { Groups | Where-Object { $_.group_id -eq $id } | Select-Object -First 1 }
function WaitFor([scriptblock]$cond, [int]$timeoutS, [string]$what) {
    $deadline = (Get-Date).AddSeconds($timeoutS)
    while ((Get-Date) -lt $deadline) {
        if (& $cond) { return $true }
        Start-Sleep -Milliseconds 500
    }
    Log "timed out waiting for: $what"
    return $false
}
function Start-Bg([string]$name, [string]$exe, [string[]]$argList) {
    $p = Start-Process -FilePath $exe -ArgumentList $argList -NoNewWindow -PassThru `
        -RedirectStandardOutput (Join-Path $WorkDir "$name.out.txt") -RedirectStandardError (Join-Path $WorkDir "$name.err.txt")
    [void]$procs.Add($p)
    return $p
}

New-Item -ItemType Directory -Force $WorkDir | Out-Null
Log "work dir $WorkDir"
try {
    # --- layout: node-a and node-b hold the model file; node-c and node-d don't.
    foreach ($n in 'a', 'b', 'c', 'd') { New-Item -ItemType Directory -Force (Join-Path $WorkDir "models-$n") | Out-Null }
    foreach ($n in 'a', 'b') { Set-Content -Path (Join-Path $WorkDir "models-$n\mock.gguf") -Value 'GGUF' }

    @"
cluster_token: "$token"
coordinator:
  host: 127.0.0.1
  port: 18100
  heartbeat_interval_s: 1
  node_timeout_s: 5
  group_start_timeout_s: 60
router:
  host: 127.0.0.1
  port: 18000
  coordinator_url: $coord
  max_queue: 4
  queue_timeout_s: 10
  request_timeout_s: 60
  api_keys: ["$apiKey"]
pools:
  - name: pair
    model_file: mock.gguf
    nodes_per_group: 2
    groups: 1
    context: 2048
    extra_args: ["--mock-load-s", "2"]
  - name: solo
    model_file: mock.gguf
    nodes_per_group: 1
    groups: 1
    parallel: 2
spares: 1
"@ | Set-Content -Path (Join-Path $WorkDir 'cluster.yaml') -Encoding ascii

    $env:QWEN_CLUSTER_TOKEN = $token
    Log 'starting xgi serve'
    Start-Bg 'xgi' $Xgi @('serve', '--config', "`"$WorkDir\cluster.yaml`"", '--log-dir', "`"$WorkDir\logs-xgi`"") | Out-Null
    Check (WaitFor { (Call GET "$coord/health").Status -eq 200 } 15 'coordinator') 'coordinator /health is up (no token needed)'

    $agents = @{}
    $i = 0
    foreach ($n in 'a', 'b', 'c', 'd') {
        $i++
        $agents["node-$n"] = Start-Bg "xi-$n" $Xi @(
            '--coordinator', $coord, '--name', "node-$n", '--host', '127.0.0.1', '--advertise-ip', '127.0.0.1',
            '--port', (18200 + $i), '--rpc-port', (50160 + $i), '--head-port', (18080 + $i), '--heartbeat-s', '1',
            '--models-dir', "`"$WorkDir\models-$n`"", '--log-dir', "`"$WorkDir\logs-$n`"",
            '--rpc-server-bin', "`"$MockRpc`"", '--llama-server-bin', "`"$MockLlama`"")
    }
    Check (WaitFor { @(Nodes | Where-Object { $_.status -eq 'alive' }).Count -eq 4 } 20 '4 nodes') '4 agents registered'
    Check ((Call GET "$coord/nodes").Status -eq 401) 'coordinator rejects calls without X-Cluster-Token'
    Check ((Call GET 'http://127.0.0.1:18201/status').Status -eq 401) 'agent rejects calls without X-Cluster-Token'
    $st = (Call GET 'http://127.0.0.1:18201/status' $null $T).Json
    Check ($st.models -contains 'mock.gguf' -and $st.cpu_physical -ge 1 -and $st.ram_total -gt 0) 'agent /status reports models and hardware'
    Check ((Call POST 'http://127.0.0.1:18201/head/start' @{ model_file = '..\cluster.yaml' } $T).Status -eq 400) 'agent rejects model_file outside the models dir'

    # --- deploy
    $d = Call POST "$coord/deploy" $null $T
    Check ($d.Status -eq 200 -and @($d.Json.groups).Count -eq 2) 'POST /deploy plans 2 groups'
    Check ((Call POST "$coord/deploy" $null $T).Status -eq 409) 'second POST /deploy returns 409'
    Check (WaitFor { @(Groups | Where-Object { $_.status -eq 'healthy' }).Count -eq 2 } 60 'healthy') 'both groups become healthy'
    $pair = Get-ClusterGroup 'pair-0'
    $solo = Get-ClusterGroup 'solo-0'
    # node-a and node-b hold the model; which one heads which pool depends on free RAM at deploy time.
    $holders = @('node-a', 'node-b')
    Check (($holders -contains $pair.head) -and @($pair.workers).Count -eq 1 -and -not ($holders -contains $pair.workers[0])) "pair-0 head=$($pair.head) holds the model; worker $($pair.workers -join ',') does not"
    Check (($holders -contains $solo.head) -and $solo.head -ne $pair.head -and $solo.slots -eq 2) "solo-0 head=$($solo.head) with 2 slots"
    $soloHead = $solo.head
    $soloAgent = 'http://127.0.0.1:' + (18200 + 1 + [array]::IndexOf(@('node-a', 'node-b', 'node-c', 'node-d'), $soloHead))

    # --- router
    Check (WaitFor { @(Items (Call GET "$router/v1/models" $null $K).Json.data).Count -eq 2 } 15 'models') 'router lists both pools in /v1/models'
    Check ((Call POST "$router/v1/chat/completions" @{ model = 'pair'; messages = @(@{ role = 'user'; content = 'hi' }) }).Status -eq 401) 'router requires an API key'
    $r = Call POST "$router/v1/chat/completions" @{ model = 'nope'; messages = @() } $K
    Check ($r.Status -eq 503 -and $r.Json.error.message -eq 'model unavailable') 'unknown model -> 503 model unavailable'
    $r = Call POST "$router/v1/chat/completions" @{ model = 'pair'; messages = @(@{ role = 'user'; content = 'hi' }) } $K
    Check ($r.Status -eq 200 -and $r.Json.choices[0].message.content -like 'Hello from mock*') 'non-streaming chat completion via pair'
    Check ($r.Headers['X-Qwen-Group'] -eq 'pair-0' -and $r.Headers['X-Qwen-Pool'] -eq 'pair' -and $null -ne $r.Headers['X-Qwen-Queue-Ms']) 'X-Qwen-* response headers'
    $r = Call POST "$router/v1/chat/completions" @{ model = 'solo'; stream = $true; messages = @(@{ role = 'user'; content = 'hi' }) } $K
    Check ($r.Status -eq 200 -and $r.Text -match 'data: ' -and $r.Text -match '\[DONE\]') 'streaming chat completion (SSE) via solo'
    $r = Call POST "$router/v1/completions" @{ model = 'solo'; prompt = 'x' } $K
    Check ($r.Status -eq 200 -and $r.Json.choices[0].text -like 'Hello*') '/v1/completions passthrough'
    $h = (Call GET "$router/health").Json
    Check ($h.pools.pair.healthy_groups -eq 1 -and $h.pools.solo.slots -eq 2) 'router /health shows per-pool counts'

    # --- failover: kill the worker's agent (its rpc-server dies with it)
    $victim = $pair.workers[0]
    Log "killing agent $victim (worker of pair-0)"
    Stop-Process -Id $agents[$victim].Id -Force
    Check (WaitFor { (Get-ClusterGroup 'pair-0').status -ne 'healthy' } 20 'degraded') 'pair-0 leaves healthy after its worker dies'
    Check (WaitFor { $g = Get-ClusterGroup 'pair-0'; $g.status -eq 'healthy' -and $g.workers[0] -ne $victim } 60 'repaired') 'pair-0 repaired onto the spare'
    Check ((Nodes | Where-Object { $_.node_id -eq $victim }).status -eq 'dead') "$victim is marked dead"
    Start-Sleep -Seconds 4  # router poll interval
    $r = Call POST "$router/v1/chat/completions" @{ model = 'pair'; messages = @(@{ role = 'user'; content = 'again' }) } $K
    Check ($r.Status -eq 200) 'pair serves again after repair'

    # --- drain the solo head: no other node holds the model -> unplaced; undrain -> healthy
    $dr = Call POST "$coord/drain/$soloHead" $null $T
    Check ($dr.Status -eq 200 -and $dr.Json.draining -eq $true) "POST /drain/$soloHead"
    Check (WaitFor { (Get-ClusterGroup 'solo-0').status -eq 'unplaced' } 20 'unplaced') 'solo-0 becomes unplaced (no free model holder)'
    Check ((Call POST "$coord/undrain/$soloHead" $null $T).Status -eq 200) "POST /undrain/$soloHead"
    Check (WaitFor { (Get-ClusterGroup 'solo-0').status -eq 'healthy' } 30 'solo healthy') 'solo-0 is placed again and healthy'
    Check ((Call POST "$coord/drain/no-such-node" $null $T).Status -eq 404) 'drain of unknown node -> 404'

    # --- process exit: kill solo-0's llama-server while its agent stays up
    $headPid = (Call GET "$soloAgent/status" $null $T).Json.processes.head.pid
    Log "killing llama-server pid $headPid on $soloHead"
    Stop-Process -Id $headPid -Force
    Check (WaitFor { (Get-ClusterGroup 'solo-0').error -like '*exited*' } 15 'exit seen') 'coordinator sees the head process exit via heartbeat'
    Check (WaitFor { $g = Get-ClusterGroup 'solo-0'; $g.status -eq 'healthy' -and $g.head -eq $soloHead } 30 'solo back') "solo-0 restarted on $soloHead"
    $newPid = (Call GET "$soloAgent/status" $null $T).Json.processes.head.pid
    Check ($newPid -and $newPid -ne $headPid) 'a new llama-server process is running'

    # --- restart
    $rs = Call POST "$coord/groups/pair-0/restart" $null $T
    Check ($rs.Status -eq 202 -and $rs.Json.status -eq 'starting') 'POST /groups/pair-0/restart -> 202 starting'
    Check (WaitFor { (Get-ClusterGroup 'pair-0').status -eq 'healthy' } 30 'restarted') 'pair-0 healthy after restart'
    Check ((Call POST "$coord/groups/nope/restart" $null $T).Status -eq 404) 'restart of unknown group -> 404'

    # --- undeploy
    $u = Call POST "$coord/undeploy" $null $T
    Check ($u.Status -eq 200 -and $u.Json.stopped -eq 2) 'POST /undeploy stops 2 groups'
    $a = (Call GET "$soloAgent/status" $null $T).Json
    Check ($a.processes.head.state -eq 'stopped' -and $a.processes.rpc.state -eq 'stopped') "$soloHead processes stopped"
    $logs = (Call GET "$soloAgent/logs/head?lines=5" $null $T).Json
    Check ($logs.role -eq 'head' -and @($logs.lines).Count -ge 1) 'GET /logs/head returns captured output'
}
finally {
    Log 'cleaning up'
    foreach ($p in $procs) { if (-not $p.HasExited) { try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch { } } }
    Get-Process -Name 'mock-llama-server', 'mock-rpc-server' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    if ($script:failures -gt 0) {
        Write-Host "logs kept in $WorkDir" -ForegroundColor Yellow
    } elseif (-not $KeepWorkDir) {
        Start-Sleep -Milliseconds 500
        Remove-Item -Recurse -Force $WorkDir -ErrorAction SilentlyContinue
    }
}
if ($script:failures -gt 0) { Write-Host "$($script:failures) check(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'e2e: all checks passed' -ForegroundColor Green
exit 0
