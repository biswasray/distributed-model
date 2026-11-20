# 02 · Node Agent

> Component spec. Runs on **every** node. Port **8200**. Implemented by **XI** (`xi` / `xi.exe`, [ADR-007](../adr/ADR-007-cpp-implementation-xgi-xi.md), portable per [ADR-008](../adr/ADR-008-cross-platform-build.md)).

## 1. Responsibilities

1. **Register** the node with the coordinator: identity, hardware, and the model files on disk.
2. Send a **heartbeat** every `--heartbeat-s` seconds (default 5) with free RAM and the state of its child processes.
3. **Supervise** at most two llama.cpp child processes:
   - `rpc` → `rpc-server` (worker role)
   - `head` → `llama-server` (head role)
4. Expose a small, **token-protected** control API that the coordinator calls.
5. Report the **language runtimes** on its `PATH`, and, only if started with `--allow-exec`, run console programs with them (§9, [ADR-009](../adr/ADR-009-web-console-and-node-code-execution.md)).
6. **Download model files** from Hugging Face or GitHub releases into its models folder when asked, with pause, resume and cancel (§11, [ADR-011](../adr/ADR-011-model-downloads-on-nodes.md)).

The agent has **no scheduling logic**. It never decides on its own to start or stop inference. It only does what the coordinator tells it.

## 2. Command-line interface

| Flag | Default | Description |
|---|---|---|
| `--config` | `./xi.yaml`, else `xi.yaml` next to `xi` | Settings file (§10). Every other flag except `--token` and `--log-level` is also a key in it. |
| `--coordinator` | *(none)* | Coordinator base URL, e.g. `http://192.168.1.10:8100`. Without it the agent starts and waits until one is set on its web page. |
| `--token` | env `QWEN_CLUSTER_TOKEN` | Shared cluster token. Never stored in `xi.yaml`. |
| `--host` / `--port` | `0.0.0.0` / `8200` | Interface and port for the agent API |
| `--name` | hostname | `node_id`. It must be unique in the cluster. |
| `--advertise-ip` | auto | The IP other nodes use to reach this one. Auto-detected from the route to the coordinator. |
| `--llama-dir` | `llama.cpp` (relative to the working directory) | Folder containing `rpc-server` and `llama-server` (`.exe` on Windows) |
| `--models-dir` | `models` | Folder scanned (recursively) for `*.gguf` files |
| `--log-dir` | `logs` | Agent and child process logs |
| `--rpc-port` | `50052` | Port `rpc-server` listens on |
| `--head-port` | `8080` | Port `llama-server` listens on |
| `--rpc-server-bin` / `--llama-server-bin` | — | Override the binary paths. Tests use `mock-rpc-server.exe` / `mock-llama-server.exe`. A `.py` path runs with `python` from `PATH`. |
| `--heartbeat-s` | `5` | Heartbeat interval |
| `--allow-exec` | off | Allow `POST /exec` (console code execution, §9) |
| `--exec-dir` | `exec` | Working directory for console programs |
| `--exec-max-timeout-s` | `300` | Longest run a console job may ask for |
| `--log-level` | `info` | `debug`, `info`, `warn` or `error` |

The agent reports its port overrides in its registration, so several agents can run on one machine during development.

## 3. Lifecycle

```
 start ──► register ──ok──► heartbeat loop ◄────────────┐
              │                 │                       │
              └── fail: retry ◄─┤ 404 (coordinator      │
                  every 5 s     │  forgot node) ────────┘ re-register
                                │
                                └─ on shutdown: stop all child processes
```

- If registration or a heartbeat fails (network error), the agent retries forever and re-registers on the next attempt.
- An **HTTP 404** reply to a heartbeat means the coordinator restarted and lost its state. The agent re-registers right away.
- Child processes are **not** stopped when the coordinator is unreachable. Inference keeps running while the control plane recovers.

## 4. Control API

All endpoints require `X-Cluster-Token` when a token is configured. Request and response schemas are in [05-protocols-and-data.md](05-protocols-and-data.md#3-agent-api).

| Method | Path | Effect |
|---|---|---|
| GET | `/status` | Full node facts: hardware, models, process states |
| POST | `/rpc/start` | (Re)start `rpc-server` |
| POST | `/head/start` | (Re)start `llama-server` with a model and a list of RPC endpoints |
| POST | `/stop` | Stop `rpc`, `head`, or `all` |
| GET | `/logs/{role}?lines=N` | The last N lines (up to 500) of a child process's output |
| POST | `/exec` | Run a console program (only with `--allow-exec`, §9) |
| GET | `/`, `/ui/…` | The agent's web page (§10). **No authentication.** |
| GET / PUT | `/agent/settings` | Read or change settings (§10) |
| GET | `/agent/state` | Registration state, facts and process states for the page |
| POST | `/agent/register` · `/agent/runtimes/detect` | Re-register now · probe language runtimes again |
| GET | `/hub/files` | List a Hugging Face repo or GitHub release (§11) |
| GET / POST | `/downloads` | List download jobs · start one (§11) |
| POST / DELETE | `/downloads/{id}/pause\|resume\|cancel` · `/downloads/{id}` | Control a download · drop it from the list |
| GET | `/models` · `/models/file/<path>` | List this node's GGUF files with sizes · send one to another node (§12) |

### 4.1 Command lines the agent builds

**Worker:**

```
rpc-server -H 0.0.0.0 -p <rpc_port> -t <threads> [-c]
```

**Head:**

```
llama-server -m <models_dir>/<model_file> --host 0.0.0.0 --port <head_port>
             -c <context × parallel> -np <parallel> -t <threads> -ngl <ngl>
             [--rpc ip1:port,ip2:port,...] [--tensor-split a,b,c] [extra_args...]
```

- `threads` defaults to the number of **physical** CPU cores.
- `-c` is multiplied by `parallel`, because llama-server splits its context evenly between slots. That keeps the context per request equal to the pool's `context`.
- `--rpc` is left out for single-node pools.

## 5. Process supervision rules

| Rule | Detail |
|---|---|
| One process per role | Starting a role that is already running stops the old process first (restart semantics). |
| Graceful stop | Terminate, wait up to 15 seconds, then kill. |
| Output capture | stdout and stderr are merged, appended to `logs\<role>.log`, and the last 500 lines are kept in memory. The agent's own log is `logs\xi.log`. |
| Windows specifics | Children start with `CREATE_NEW_PROCESS_GROUP`, so console signals don't spread to the agent. "Terminate" is `CTRL_BREAK` when the agent has a console, and otherwise a hard kill. Each child runs in a kill-on-close **job object**, so it dies with the agent even if the agent crashes. |
| Linux / macOS specifics | Children start in their own process group (`setpgid`). "Terminate" is `SIGTERM` to the group, then `SIGKILL` after the grace period. Other inherited file descriptors are closed. On Linux, `PR_SET_PDEATHSIG` kills the child if the agent dies. macOS has no equivalent, so a crashed agent can leave a child running until the next start replaces it. `exit_code` is negative (`-signal`) when a signal killed the process. |
| Exit detection | The heartbeat reports `state: exited` with `pid` and `exit_code`. A process stopped on request reports `stopped`. The **coordinator** decides what to do about it. |
| Agent shutdown | All children are stopped on Ctrl+C, console close, `SIGINT` or `SIGTERM`, and by the job object or the parent-death signal if the agent dies. |

## 6. Safety constraints

- `model_file` is a path **relative to `--models-dir`**. The agent resolves it and refuses anything outside the models folder, which blocks path traversal ([ADR-005](../adr/ADR-005-model-files-relative-to-models-dir.md)).
- Without `--allow-exec`, the agent only ever runs the two configured llama.cpp binaries (plus a `--version` probe of each language runtime at startup). With it, it also runs console programs (§9).
- Downloads (§11) only fetch from Hugging Face model repos and GitHub releases, with URLs the agent builds itself, or from another cluster machine on a private IPv4 address (LAN copy, fixed paths only). They only write inside the models folder. An existing file is never overwritten.
- The agent serves only complete `.gguf` files inside its models folder to other nodes (§12), and only with the token.
- `extra_args` are passed straight to `llama-server`. They are trusted because the caller holds the cluster token ([06-security.md](06-security.md)).

## 7. Node facts reported

`node_id`, `hostname`, `ip`, `agent_port`, `rpc_port`, `head_port`, `ram_total`, `ram_available`, `cpu_physical`, `cpu_logical`, `os`, `arch`, `cpu_model`, `disk_free`, `models[]`, `processes{rpc, head}`, `binaries{rpc_server, llama_server}`, `runtimes[]`, `exec_enabled`, `downloads{available, tool}`, `agent_version`.

## 8. Running as a service

The install scripts write `<root>\xi.yaml` (if it doesn't exist) and start `xi --config <root>\xi.yaml`, so changes made on the agent page survive restarts. On Windows, register the agent as a Task Scheduler task that runs **at startup as SYSTEM**. The command is in the [setup guide §7.6](../distributed-qwen-cluster.md#76-run-the-node-agent-as-a-startup-service). On Linux it runs as a systemd unit, and on macOS as a launchd daemon (`scripts/install-xi-service.sh`).

Also on each node: turn off sleep and hibernation, and set the Wi-Fi adapter power mode to *Maximum Performance*.

## 9. Language runtimes and console programs

At startup the agent looks for each known runtime on `PATH` and runs its version probe in parallel (5 s timeout each). A runtime counts only if the probe exits with 0, so the Microsoft Store `python` stub is skipped. On Windows, `System32\bash.exe` (the WSL launcher) is skipped too, because it can't run a Windows path; Git Bash is used if present. The list is fixed until the agent restarts.

| Language id | Executables tried | How a program runs |
|---|---|---|
| `python` | `python`, `py`, `python3` (POSIX: `python3`, `python`) | `python -X utf8 main.py` |
| `javascript` | `node` | `node main.js` |
| `powershell` / `pwsh` | `powershell` (Windows) / `pwsh` | `-NoProfile -NonInteractive -ExecutionPolicy Bypass -File main.ps1` |
| `bash` / `sh` | `bash` / `sh` (POSIX) | `bash main.sh` |
| `cmd` | `cmd` (Windows) | `cmd /d /c main.cmd` |
| `ruby`, `perl`, `php`, `lua` | same name | `<exe> main.<ext>` |
| `go` | `go` | `go run main.go` |
| `java` | `java` (11+) | `java Main.java` (single-file source launch) |
| `c` / `cpp` / `rust` | `gcc`, `clang`, `cc` / `g++`, `clang++`, `c++` / `rustc` | compile to `main[.exe]`, then run it |

`POST /exec` (schema in [05 §3](05-protocols-and-data.md#3-agent-api)):

1. Returns `403` unless the agent runs with `--allow-exec`, `400` if the language isn't detected, and `429` if 2 jobs are already running.
2. Writes the code to `<exec-dir>/.jobs/<random id>/main.<ext>`. `.ps1` files get a UTF-8 BOM for Windows PowerShell 5.1, and `.cmd` files get CRLF line endings.
3. Runs it with `<exec-dir>` as the working directory and empty stdin, under the same supervision as llama.cpp (§5): a kill-on-close job object on Windows, and a process group on POSIX. At the timeout, the whole process tree is killed.
4. Returns the exit code, duration and the last 500 output lines, then deletes the job folder. Files the program writes to `<exec-dir>` stay.

Each run is logged in `xi.log`, and its output is appended to `logs/exec.log`.

> ⚠️ Programs run with the agent's account and rights. The scheduled task runs as **SYSTEM**, so on Windows a console program can do anything on that PC. Only use `--allow-exec` on nodes where that is acceptable, or run the agent as a dedicated low-privilege user ([06-security.md](06-security.md) T10).

## 10. Web page and runtime settings

[ADR-010](../adr/ADR-010-agent-web-page-and-runtime-settings.md). Open `http://<node>:8200/ui/` (on the node itself: `http://127.0.0.1:8200/ui/`). The page is plain HTML/CSS/JS embedded in `xi` (with XGI's stylesheet), loads without a token, and sends `X-Cluster-Token` on every call. The XGI console's node panel links to it.

- **Status tab:** coordinator link (state, last heartbeat, last error, *Re-register*), system facts, the llama.cpp processes and binaries, model files, languages (*Detect again*), and the last 50–500 lines of `llama-server` or `rpc-server` output.
- **Settings tab:** a form over the `xi.yaml` keys ([05 §5.2](05-protocols-and-data.md#52-xiyaml-node-agent-settings)). Each section shows *applies now* or *after restart*. A setting also passed as a flag is marked with that flag: the flag wins again at the next start.

| Applies at once | After an XI restart |
|---|---|
| `coordinator`, `name`, `advertise_ip`, `heartbeat_s`, `llama_dir`, `rpc_server_bin`, `llama_server_bin`, `models_dir`, `allow_exec`, `exec_dir`, `exec_max_timeout_s`, and the token (not saved) | `host`, `port`, `log_dir`, `rpc_port`, `head_port` |

How a save works:

1. The new settings are validated and written to `xi.yaml` atomically (the previous file is kept as `xi.yaml.bak`).
2. The live keys replace the running ones under a lock. Request handlers read a snapshot, so a request sees either the old or the new settings, never a mix.
3. The control loop wakes up and registers again (new coordinator, name, IP, models or exec state). Renaming the node, or moving it to another coordinator, leaves the old record to go dead after `node_timeout_s`, and its group is repaired.
4. llama.cpp folder and binary changes apply to the next `/rpc/start` or `/head/start`. Running processes are not touched.

**`allow_exec` can only be turned on locally:** from a loopback address (a browser on the node), with `--allow-exec`, or by editing `xi.yaml`. A remote save that turns it on gets `403`. Turning it off works from anywhere, and takes effect at once.

## 11. Model downloads

[ADR-011](../adr/ADR-011-model-downloads-on-nodes.md). The coordinator's console (Models tab) and `xgi download` start downloads here; the schemas are in [05 §3](05-protocols-and-data.md#model-downloads-adr-011-1).

The agent looks for `curl` on `PATH` at startup (`curl.exe` is part of Windows 10 1803 and later, Linux and macOS) and reports `downloads.available`. Without it, the download endpoints answer `503`.

A job is a source (`huggingface` or `github`), a repo, a ref and up to 64 files:

1. **Starting:** the agent lists the repo once (Hugging Face tree API, or the GitHub release; `latest` resolves to its tag) to learn file sizes and check the files exist. Then it checks there is room on the disk for what is left, plus 256 MB.
2. **Downloading**, one file at a time: `curl -L --fail -C - -o <target>.part <url>`, supervised like llama.cpp (§5). The agent reads the `.part` file's size four times a second for progress and speed. On success, with the expected size, it renames `.part` to the target. A target that already exists with the same size is skipped (`present`); with another size the job fails rather than overwrite it.
3. **Failures** (network error, stall under 1 byte/s for 2 minutes, wrong size) retry up to 5 times, 3 s × attempt apart, resuming the `.part` file. Progress resets the count. HTTP 401, 403 and 404 fail at once with a message that names the token variable to set.
4. **Pause** kills curl and keeps the `.part` file. **Resume** (also of a failed job) starts curl again with `-C -`, which continues with an HTTP Range request. **Cancel** kills curl and deletes the `.part` files of unfinished files. When the agent stops, running jobs are paused; after a restart, starting the same download continues the `.part` file.
5. After each completed file the agent **re-registers**, so the coordinator's `models` list (and placement) includes the file straight away.

At most 3 jobs download at once per node, and two unfinished jobs can't write the same file. Tokens for gated or private repos come from the agent's environment: `HF_TOKEN` (or `HUGGING_FACE_HUB_TOKEN`) and `GITHUB_TOKEN` (or `GH_TOKEN`). The agent passes them to curl in a short-lived header file under `<log_dir>/downloads/`, not on the command line. curl's errors go to `logs/downloads.log`, and each job's steps to `xi.log`.

**LAN copies** ([ADR-012](../adr/ADR-012-lan-model-copies-between-nodes.md)). A job with source `peer` copies `.gguf` files from another node, or from the XGI machine, instead of a hub. It works the same way, with these differences:

- The listing step calls `GET <peer>/models`, and each file comes from `GET <peer>/models/file/<path>`. Both carry `X-Cluster-Token` in a header file. curl doesn't follow redirects, so the token can't leak to another host.
- `peer_url` must be `@xgi` (meaning this agent's `coordinator` URL) or `http://<private IPv4>:<port>`. The console resolves node ids and `auto` for the operator (03 §11).
- The file keeps its folder: `<models_dir>/<dest_dir>/<path on the peer>`.
- A busy peer (`429`) isn't a failure. The job shows a `note` and tries again every 15 s.

## 12. Serving model files to other nodes

[ADR-012](../adr/ADR-012-lan-model-copies-between-nodes.md). Every agent serves its models folder read-only, so other nodes can copy a model over the LAN instead of downloading it again (§11, "LAN copies"):

- `GET /models` lists the `.gguf` files under `--models-dir` with their sizes. The same list goes to the coordinator as `model_sizes` in `/register`.
- `GET /models/file/<path>` sends one file and honours HTTP `Range`, so the receiver resumes after a pause, a Wi-Fi drop or a restart. The file is read in 1 MiB chunks; nothing is held in RAM.
- Only complete `.gguf` files that resolve inside the models folder are served (the same check as `model_file`, [ADR-005](../adr/ADR-005-model-files-relative-to-models-dir.md)). `.part` files and anything outside the folder are refused.
- At most **2** files are sent at once. Another request gets `429` with `Retry-After: 20`, which keeps the load on a head that is also running llama.cpp bounded. Each transfer is logged in `xi.log` with the receiver's address and the bytes sent.
- Both endpoints need `X-Cluster-Token`, like the rest of the agent API, and use port 8200. No extra firewall rule is needed.

Schemas are in [05 §3](05-protocols-and-data.md#model-files-lan-copies-adr-012).
