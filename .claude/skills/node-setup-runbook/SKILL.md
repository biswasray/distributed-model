---
name: node-setup-runbook
description: Step-by-step runbook for preparing a Windows PC as a Qwen cluster node (head or worker) or the control machine; use when the user asks how to set up, add, or replace a machine, or when writing/updating setup instructions in the docs.
---

# Node setup runbook

Walk the user through these steps in order. Ask which role the machine will have: **worker**, **head candidate** (it will hold model files), or **control machine** (router and coordinator). Commands are PowerShell. Steps marked 🔒 need an **elevated** prompt.

## 1. Preflight checks

| Check | How | Pass |
|---|---|---|
| OS | `winver` | Windows 10/11 64-bit |
| Free RAM | `Get-CimInstance Win32_OperatingSystem \| Select FreePhysicalMemory` | ≥ 7 GB free while idle |
| CPU AVX2 | Check the CPU model's spec sheet | AVX2 present (for the standard llama.cpp CPU build) |
| Disk | `Get-PSDrive C` | Worker: ≥ 10 GB · head: model size + 10 GB |
| Network | `Test-NetConnection <control-ip> -Port 8100` | `TcpTestSucceeded : True` (once the coordinator is running) |
| Static IP | DHCP reservation on the router or AP | IP stays the same after reboot |

## 2. Power and network settings 🔒

- Turn off sleep and hibernation: `powercfg /change standby-timeout-ac 0` · `powercfg /hibernate off`
- Wi-Fi adapter → Power Management → don't allow the computer to turn it off. Set the power plan wireless setting to *Maximum Performance*.
- Set the network profile to **Private**: `Set-NetConnectionProfile -InterfaceAlias "Wi-Fi" -NetworkCategory Private`
- Prefer **5 GHz**. Keep all members of a group on the same access point.

## 3. Install the software

1. Create `C:\qwen-cluster\` with the subfolders `llama.cpp\`, `models\`, `logs\`.
2. llama.cpp: download the official Windows CPU release from GitHub and extract it to `C:\qwen-cluster\llama.cpp\`. **Use the same release tag as every other node.** Check that `rpc-server.exe` and `llama-server.exe` are there.
3. Copy `bin\xi.exe` and `scripts\install-xi-task.ps1` to `C:\qwen-cluster` (see the operator guide §7.5). No runtime is needed: XI is a self-contained executable. (Linux/macOS nodes: copy `bin/xi` and run `scripts/install-xi-service.sh` as root.)

## 4. Firewall 🔒

Allow only the cluster subnet:

```powershell
$subnet = "192.168.1.0/24"
New-NetFirewallRule -DisplayName "Qwen RPC"   -Direction Inbound -Protocol TCP -LocalPort 50052 -RemoteAddress $subnet -Action Allow -Profile Private
New-NetFirewallRule -DisplayName "Qwen Agent" -Direction Inbound -Protocol TCP -LocalPort 8200  -RemoteAddress $subnet -Action Allow -Profile Private
New-NetFirewallRule -DisplayName "Qwen Head"  -Direction Inbound -Protocol TCP -LocalPort 8080  -RemoteAddress $subnet -Action Allow -Profile Private
```

Control machine: also open 8100 (cluster subnet) and 8000 (client subnet).

## 5. Cluster token 🔒

```powershell
[Environment]::SetEnvironmentVariable("QWEN_CLUSTER_TOKEN", "<same secret on every machine>", "Machine")
```

Never write the token into docs, chat, or committed files.

## 6. Model files (head candidates only)

Copy the GGUF files into `C:\qwen-cluster\models\` from a USB drive or a wired share. Keep the **file names exactly** as written in the pool's `model_file`.

## 7. Start the agent at boot 🔒

Register the scheduled task as shown in the operator guide §7.6, then reboot.

## 8. Verify

1. `Get-ScheduledTask QwenNodeAgent` → State `Running`
2. On the control machine: `GET /nodes` lists this machine as `alive`, and `models` shows the staged files.
3. After deployment: `GET /groups` shows the group this node belongs to as `healthy`.

## Replacing a machine

Drain the old node (`POST /drain/<node_id>`) if it's still alive. Set up the new one with the steps above, using the same model files if it was a head. The coordinator uses it as a spare automatically.
