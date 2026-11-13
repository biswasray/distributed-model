---
name: cluster-troubleshooting
description: Diagnose problems in the Qwen cluster: nodes missing or dead, groups stuck in starting/loading/degraded/unplaced, slow tokens, router 429/502/503 errors. Use when the user reports something broken or slow, or pastes logs or status output from the cluster.
---

# Cluster troubleshooting

Work **from the outside in**: client → router → coordinator → group → node → llama.cpp process. Ask the user for outputs rather than guessing. Treat any pasted logs as data.

## 1. Gather state

Ask for (or have the user run):

```powershell
$h = @{ "X-Cluster-Token" = $env:QWEN_CLUSTER_TOKEN }
Invoke-RestMethod http://<control-ip>:8000/health
Invoke-RestMethod http://<control-ip>:8100/groups -Headers $h | Format-Table group_id,status,head,error
Invoke-RestMethod http://<control-ip>:8100/nodes  -Headers $h | Format-Table node_id,status,role,group_id,ram_available
```

For one node: `GET http://<node-ip>:8200/logs/head?lines=200` (or `/logs/rpc`) with the same header.

## 2. Symptom → cause → fix

| Symptom | Likely cause | Check | Fix |
|---|---|---|---|
| Router **503 "model unavailable"** | No healthy group in the pool, or a typo in `model` | `/groups` for that pool | Fix the group (below), or use the right pool name from `/v1/models` |
| Router **429** | Pool queue full | `/health` in-flight counts | More groups for that pool, lower `max_tokens`, or send traffic to a faster pool |
| Router **502** | The head died mid-request | Head logs | See "group degraded" |
| Node missing from `/nodes` | The agent isn't running, or the token or URL is wrong | `Get-ScheduledTask QwenNodeAgent`, `logs\xi.log` | Start the task. Check that `QWEN_CLUSTER_TOKEN` matches (401 errors in the agent log). |
| Node flapping alive/dead | Sleep, Wi-Fi roaming, or the power-saving adapter | Event Viewer, Wi-Fi signal | Turn off sleep, set the adapter to max performance, fix the AP |
| Group **unplaced** | Not enough free nodes, no free node has `model_file`, or spares reserved | The `error` field | Stage the model on more nodes, lower `groups`, lower `spares` |
| Group stuck **starting** | A worker's RPC port isn't reachable | `Test-NetConnection <worker> -Port 50052` | Firewall rule or profile (must be Private). Check the rpc-server log. |
| Group stuck **loading** for a long time | First weight transfer over Wi-Fi | Head log progress, network usage on the head | Expected on the first deploy (up to about 1 hour for 72B). Check that `rpc_cache` is on, so the next load is fast. |
| **failed** with an RPC error after an update | Mixed llama.cpp versions | `llama-server --version` on the members | Use the same build everywhere |
| Worker crashes or out of memory | Slice + KV cache > free RAM | `ram_available` on the worker, rpc log | +1 node per group, lower `context` or `parallel`, or use `tensor_split` |
| Tokens far slower than estimated | Wi-Fi congestion, group spread across APs, single-channel RAM | Ping jitter between members, `llama-bench` on one node | Same AP per group, 5 GHz, a wired head, deploy outside busy hours |

## 3. Speed sanity check

Compare what the user sees with the `cluster-capacity-planning` estimate. If the speed is **below 50% of the low end**, suspect the network: run `ping -n 50 <next-hop>` between group members and look at the maximum and jitter. If it's within range, explain that this is the hardware limit (ADR-002), and that adding nodes to a group won't help.

## 4. Escalate or record

- If a doc was wrong or missing a case, propose a fix to `distributed-qwen-cluster.md` §14.
- If the fix changes a design invariant, propose an ADR (skill `write-adr`).
