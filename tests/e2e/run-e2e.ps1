<#
.SYNOPSIS
  End-to-end test: xgi serve + 4 xi agents on localhost, driving mock llama.cpp binaries.

  Covers: registration, auth (cluster token + API keys), deploy/409, group start sequence,
  routing (stream + non-stream), unknown model, web console (UI, generate, exec), model downloads
  (mock Hugging Face/GitHub: progress, pause/resume, cancel, CLI),
  worker failure -> repair onto the spare, drain -> unplaced -> undrain -> healthy, restart, undeploy.
#>
param(
    [Parameter(Mandatory)][string]$Xgi,
    [Parameter(Mandatory)][string]$Xi,
    [Parameter(Mandatory)][string]$MockLlama,
    [Parameter(Mandatory)][string]$MockRpc,
    [Parameter(Mandatory)][string]$MockHub,
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
console:
  allow_exec: true
  router_url: $router
  model: solo
  generate_timeout_s: 30
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
    # The agents download from a local mock instead of huggingface.co / github.com.
    Start-Bg 'mock-hub' $MockHub @('--port', '18900') | Out-Null
    $env:XI_HF_BASE = 'http://127.0.0.1:18900'
    $env:XI_GITHUB_BASE = 'http://127.0.0.1:18900'
    $env:XI_GITHUB_API_BASE = 'http://127.0.0.1:18900/api'
    Log 'starting xgi serve'
    Start-Bg 'xgi' $Xgi @('serve', '--config', "`"$WorkDir\cluster.yaml`"", '--log-dir', "`"$WorkDir\logs-xgi`"") | Out-Null
    Check (WaitFor { (Call GET "$coord/health").Status -eq 200 } 15 'coordinator') 'coordinator /health is up (no token needed)'

    $agents = @{}
    $i = 0
    foreach ($n in 'a', 'b', 'c', 'd') {
        $i++
        $execFlags = if ($n -eq 'a') { @('--allow-exec', '--exec-dir', "`"$WorkDir\exec-a`"") } else { @() }
        $agents["node-$n"] = Start-Bg "xi-$n" $Xi ($execFlags + @(
            '--coordinator', $coord, '--name', "node-$n", '--host', '127.0.0.1', '--advertise-ip', '127.0.0.1',
            '--port', (18200 + $i), '--rpc-port', (50160 + $i), '--head-port', (18080 + $i), '--heartbeat-s', '1',
            '--models-dir', "`"$WorkDir\models-$n`"", '--log-dir', "`"$WorkDir\logs-$n`"", '--config', "`"$WorkDir\xi-$n.yaml`"",
            '--rpc-server-bin', "`"$MockRpc`"", '--llama-server-bin', "`"$MockLlama`""))
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

    # --- web console (ADR-009)
    $ui = Call GET "$coord/ui/"
    Check ($ui.Status -eq 200 -and $ui.Text -match '<title>XGI Console</title>') 'GET /ui/ serves the console page without a token'
    Check ((Call GET "$coord/ui/app.js").Status -eq 200 -and (Call GET "$coord/ui/nope.js").Status -eq 404) 'console static files: app.js 200, unknown 404'
    Check ((Call GET "$coord/console/info").Status -eq 401) 'console API requires X-Cluster-Token'
    $info = (Call GET "$coord/console/info" $null $T).Json
    Check ($info.allow_exec -eq $true -and $info.model -eq 'solo') 'GET /console/info'
    $na = Nodes | Where-Object { $_.node_id -eq 'node-a' }
    $nb = Nodes | Where-Object { $_.node_id -eq 'node-b' }
    Check ($na.exec_enabled -eq $true -and $nb.exec_enabled -eq $false -and $na.arch -and $null -ne $na.runtimes) 'node facts report exec_enabled, arch and runtimes'
    $onWindows = $env:OS -eq 'Windows_NT'
    $lang = if ($onWindows) { 'cmd' } else { 'sh' }
    $code = if ($onWindows) { "@echo off`necho console-ok %CD%`nexit /b 0" } else { "echo console-ok `$PWD`nexit 0" }
    Check (@($na.runtimes | Where-Object { $_.language -eq $lang }).Count -eq 1) "node-a detected the $lang runtime"
    $g = Call POST "$coord/console/generate" @{ node_id = 'node-a'; language = $lang; prompt = 'print ok' } $T
    Check ($g.Status -eq 200 -and $g.Json.code -like 'Hello from mock*' -and $g.Json.group -eq 'solo-0') 'POST /console/generate goes through the router to the solo pool'
    Check ((Call POST "$coord/console/generate" @{ language = $lang; prompt = '' } $T).Status -eq 400) 'generate with an empty prompt -> 400'
    $x = Call POST "$coord/console/exec" @{ node_id = 'node-a'; language = $lang; code = $code; timeout_s = 20 } $T
    Check ($x.Status -eq 200 -and $x.Json.exit_code -eq 0 -and ($x.Json.output -join ' ') -match 'console-ok .*exec-a') 'POST /console/exec runs code on node-a in its --exec-dir'
    Check ((Call POST "$coord/console/exec" @{ node_id = 'node-b'; language = $lang; code = $code } $T).Status -eq 403) 'exec on a node without --allow-exec -> 403'
    Check ((Call POST "$coord/console/exec" @{ node_id = 'node-a'; language = 'cobol'; code = 'x' } $T).Status -eq 400) 'exec with an unavailable language -> 400'
    Check ((Call POST 'http://127.0.0.1:18202/exec' @{ language = $lang; code = $code } $T).Status -eq 403) 'agent /exec is off without --allow-exec'

    # --- console configuration editor
    $cf = Call GET "$coord/console/config" $null $T
    Check ($cf.Status -eq 200 -and $cf.Json.file_exists -and $cf.Json.path -like '*cluster.yaml' -and $null -eq $cf.Json.config.cluster_token) 'GET /console/config (token not exposed in the form data)'
    Check ((Call GET "$coord/console/config").Status -eq 401) 'config API requires X-Cluster-Token'
    $bad = Call POST "$coord/console/config/check" @{ yaml = "pools: [{name: x}]" } $T
    Check ($bad.Status -eq 400 -and $bad.Json.error) 'invalid config is rejected with an error message'
    $form = $cf.Json.config
    $form.console.max_tokens = 777
    $form.router.max_queue = 5
    $sv = Call PUT "$coord/console/config" @{ config = $form } $T
    Check ($sv.Status -eq 200 -and @($sv.Json.live) -contains 'console.max_tokens' -and @($sv.Json.restart) -contains 'router.max_queue') 'PUT /console/config saves; reports live vs restart changes'
    $cf2 = (Call GET "$coord/console/config" $null $T).Json
    Check ($cf2.running.console.max_tokens -eq 777 -and $cf2.running.router.max_queue -eq 4 -and @($cf2.restart_pending) -contains 'router.max_queue') 'live setting applied, restart-only setting pending'
    Check ((Get-Content (Join-Path $WorkDir 'cluster.yaml') -Raw) -match 'e2e-token' -and (Test-Path (Join-Path $WorkDir 'cluster.yaml.bak'))) 'saved file keeps the token and a .bak copy'

    # --- XI agent page and runtime settings (ADR-010)
    $agentB = 'http://127.0.0.1:18202'
    $xp = Call GET "$agentB/ui/"
    Check ($xp.Status -eq 200 -and $xp.Text -match '<title>XI Agent</title>' -and (Call GET "$agentB/ui/app.css").Status -eq 200) 'agent serves its web page (and the shared stylesheet) without a token'
    Check ((Call GET "$agentB/agent/state").Status -eq 401) 'agent page API requires X-Cluster-Token'
    $as = (Call GET "$agentB/agent/state" $null $T).Json
    Check ($as.link.state -eq 'registered' -and $as.node.node_id -eq 'node-b') 'GET /agent/state: registered with the coordinator'
    $sg = (Call GET "$agentB/agent/settings" $null $T).Json
    Check (-not $sg.file_exists -and $sg.cli_flags.heartbeat_s -eq '--heartbeat-s' -and $sg.local_request) 'GET /agent/settings shows flags and that xi.yaml is not created yet'
    $ps = Call PUT "$agentB/agent/settings" @{ settings = @{ exec_max_timeout_s = 120; rpc_port = 50999 } } $T
    Check ($ps.Status -eq 200 -and @($ps.Json.live) -contains 'exec_max_timeout_s' -and @($ps.Json.restart) -contains 'rpc_port' -and (Test-Path (Join-Path $WorkDir 'xi-b.yaml'))) 'PUT /agent/settings applies live keys, defers restart keys, creates xi.yaml'
    $sg = (Call GET "$agentB/agent/settings" $null $T).Json
    Check ($sg.running.exec_max_timeout_s -eq 120 -and $sg.running.rpc_port -eq 50162 -and @($sg.restart_pending) -contains 'rpc_port') 'running settings updated; rpc_port waits for a restart'
    Check ((Call PUT "$agentB/agent/settings" @{ settings = @{ port = 'x' } } $T).Status -eq 400) 'invalid agent setting -> 400'
    $hb = Call PUT "$agentB/agent/settings" @{ settings = @{ heartbeat_s = 1.5 } } $T
    Check ($hb.Status -eq 200 -and @($hb.Json.warnings).Count -ge 1) 'changing a setting that is also a flag warns that the flag wins at restart'
    $en = Call PUT "$agentB/agent/settings" @{ settings = @{ allow_exec = $true } } $T
    Check ($en.Status -eq 200 -and (WaitFor { (Nodes | Where-Object { $_.node_id -eq 'node-b' }).exec_enabled -eq $true } 10 'exec on')) 'exec turned on from this machine (loopback) reaches the coordinator'
    $off = Call PUT "$agentB/agent/settings" @{ settings = @{ allow_exec = $false } } $T
    Check ($off.Status -eq 200 -and (Call POST "$agentB/exec" @{ language = $lang; code = $code } $T).Status -eq 403) 'exec turned off again takes effect at once'

    # --- model downloads from the (mock) hubs, driven through the console (ADR-011)
    $agentC = 'http://127.0.0.1:18203'
    $nc = Nodes | Where-Object { $_.node_id -eq 'node-c' }
    Check ($nc.downloads.available -eq $true) 'node facts report that downloads are available (curl found)'
    $lf = Call GET "$coord/console/hub/files?node_id=node-c&source=huggingface&repo=test/mock" $null $T
    Check ($lf.Status -eq 200 -and $lf.Json.ref -eq 'main' -and @($lf.Json.files).Count -eq 4 -and $lf.Json.total_files -eq 5) 'GET /console/hub/files lists the GGUF files (via the node)'
    $lfAll = (Call GET "$coord/console/hub/files?node_id=node-c&source=huggingface&repo=test/mock&all=true" $null $T).Json
    Check (@($lfAll.files).Count -eq 5) 'hub listing with all=true includes non-GGUF files'
    $nf = Call GET "$coord/console/hub/files?node_id=node-c&source=hf&repo=no/such" $null $T
    Check ($nf.Status -eq 404 -and $nf.Json.error.message -like '*not found*') 'listing an unknown repo -> 404 with a clear message'
    Check ((Call POST "$coord/console/downloads" @{ node_ids = @('node-c'); source = 'hf'; repo = 'test/mock'; files = @('../x.gguf') } $T).Status -eq 400) 'download with a path outside the models folder -> 400'

    $dl = Call POST "$coord/console/downloads" @{ node_ids = @('node-c', 'node-d'); source = 'huggingface'; repo = 'test/mock'; files = @('mock-a.gguf') } $T
    Check ($dl.Status -eq 200 -and @($dl.Json.started).Count -eq 2) 'POST /console/downloads starts the download on 2 nodes'
    $jc = ($dl.Json.started | Where-Object { $_.node_id -eq 'node-c' }).id
    $jd = ($dl.Json.started | Where-Object { $_.node_id -eq 'node-d' }).id
    function DlJob($node, $id) { Items (Call GET "$coord/console/downloads?node_id=$node" $null $T).Json.downloads | Where-Object { $_.id -eq $id } }
    Check ((Call POST "$coord/console/downloads" @{ node_ids = @('node-c'); source = 'hf'; repo = 'test/mock'; files = @('mock-a.gguf') } $T).Status -eq 409) 'a second download of the same file on the same node -> 409'
    Check (WaitFor { $j = DlJob 'node-c' $jc; $j.state -eq 'downloading' -and $j.done_bytes -gt 0 } 20 'progress') 'download reports progress (done_bytes, total_bytes)'
    $pz = Call POST "$coord/console/downloads/node-c/$jc/pause" @{} $T
    Check ($pz.Status -eq 200 -and (WaitFor { (DlJob 'node-c' $jc).state -eq 'paused' } 10 'paused')) 'pause stops the download'
    $partFile = Join-Path $WorkDir 'models-c\mock-a.gguf.part'
    $kept = if (Test-Path $partFile) { (Get-Item $partFile).Length } else { 0 }
    Start-Sleep -Milliseconds 800
    $after = if (Test-Path $partFile) { (Get-Item $partFile).Length } else { -1 }
    Check ($kept -gt 0 -and $after -eq $kept -and $kept -lt 8000000) "paused download keeps its .part file ($kept bytes) and stops growing"
    Check ((Call POST "$coord/console/downloads/node-c/$jc/resume" @{} $T).Status -eq 200) 'resume is accepted'
    Check (WaitFor { (DlJob 'node-c' $jc).state -eq 'completed' } 40 'completed c') 'resumed download completes'
    $mf = Join-Path $WorkDir 'models-c\mock-a.gguf'
    $okBytes = (Test-Path $mf) -and (Get-Item $mf).Length -eq 8000000 -and -not (Test-Path $partFile)
    if ($okBytes) {  # resumed with a Range request: the bytes must still be in order
        $fs = [IO.File]::OpenRead($mf); $buf = New-Object byte[] 16; [void]$fs.Seek(7999984, 'Begin'); [void]$fs.Read($buf, 0, 16); $fs.Close()
        $okBytes = (0..15 | Where-Object { $buf[$_] -ne ((7999984 + $_) % 251) }).Count -eq 0
    }
    Check $okBytes 'downloaded file has the right size and content after pause/resume; .part removed'
    Check (WaitFor { (DlJob 'node-d' $jd).state -eq 'completed' } 40 'completed d') 'the same download completes on node-d'
    Check (WaitFor { (Nodes | Where-Object { $_.node_id -eq 'node-c' }).models -contains 'mock-a.gguf' } 10 'model known') 'coordinator learns about the new model file (agent re-registers)'
    $again = Call POST "$coord/console/downloads" @{ node_ids = @('node-c'); source = 'hf'; repo = 'test/mock'; files = @('mock-a.gguf') } $T
    $ja = $again.Json.started[0].id
    Check ((WaitFor { (DlJob 'node-c' $ja).state -eq 'completed' } 10 'present') -and (DlJob 'node-c' $ja).files[0].state -eq 'present') 'downloading a file that is already there completes as "present"'

    $sp = Call POST "$coord/console/downloads" @{ node_ids = @('node-c'); source = 'hf'; repo = 'test/mock'; dest_dir = 'split'; files = @('split/mock-b-00001-of-00002.gguf', 'split/mock-b-00002-of-00002.gguf') } $T
    $js = $sp.Json.started[0].id
    Check (WaitFor { $j = DlJob 'node-c' $js; $j.state -eq 'downloading' -and $j.done_bytes -gt 0 } 20 'split progress') 'split GGUF (2 files) downloads into a folder'
    Check ((Call POST "$coord/console/downloads/node-c/$js/cancel" @{} $T).Status -eq 200 -and (WaitFor { (DlJob 'node-c' $js).state -eq 'canceled' } 10 'canceled')) 'cancel stops the download'
    Check (@(Get-ChildItem (Join-Path $WorkDir 'models-c\split') -Filter '*.part' -ErrorAction SilentlyContinue).Count -eq 0) 'cancel deletes the partial files'
    Check ((Call DELETE "$coord/console/downloads/node-c/$js" $null $T).Status -eq 200 -and -not (DlJob 'node-c' $js)) 'DELETE removes a finished download from the list'

    $gh = Call POST "$coord/console/downloads" @{ node_ids = @('node-c'); source = 'github'; repo = 'test/mock'; files = @('mock-gh.gguf') } $T
    $jg = $gh.Json.started[0].id
    Check ((WaitFor { (DlJob 'node-c' $jg).state -eq 'completed' } 20 'gh') -and (DlJob 'node-c' $jg).ref -eq 'v1' -and (Get-Item (Join-Path $WorkDir 'models-c\mock-gh.gguf')).Length -eq 500000) 'GitHub release asset downloads (latest -> v1, through a redirect)'

    $cliOut = & $Xgi download start hf test/mock mock-c.gguf --node node-d --coordinator $coord --token $token 2>&1
    Check ($LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $WorkDir 'models-d\mock-c.gguf'))) "xgi download start (CLI) downloads and waits: $(($cliOut | Select-Object -Last 1))"
    $cliList = (& $Xgi download list --node node-c --coordinator $coord --token $token 2>&1) -join "`n"
    Check ($LASTEXITCODE -eq 0 -and $cliList -match "node-c/$jc\s+completed") 'xgi download list shows the jobs'
    $cliFiles = (& $Xgi download files hf test/mock --node node-c --coordinator $coord --token $token 2>&1) -join "`n"
    Check ($LASTEXITCODE -eq 0 -and $cliFiles -match 'mock-a\.gguf') 'xgi download files lists the repo'

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
    Get-Process -Name 'mock-llama-server', 'mock-rpc-server', 'mock-hub' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
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
