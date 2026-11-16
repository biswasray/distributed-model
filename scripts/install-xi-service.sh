#!/bin/sh
# Registers XI (xi) as a boot-time service on Linux (systemd) or macOS (launchd).
# The Linux/macOS counterpart of install-xi-task.ps1 (architecture/02-node-agent.md §8).
#
# Usage (as root):  sudo ./install-xi-service.sh [http://192.168.1.10:8100] [/opt/qwen-cluster]
# Settings live in <root>/xi.yaml (created if missing; the agent web page edits it). Without a
# coordinator URL the agent starts and waits: set it on http://127.0.0.1:8200/ui/.
# Set XI_ALLOW_EXEC=1 to let the XGI web console run code on this node (ADR-009; runs as root).
# The token is read from /etc/qwen-cluster.env (QWEN_CLUSTER_TOKEN=...), which this script
# creates with mode 600 if it is missing.
set -eu

COORDINATOR="${1:-}"
ROOT="${2:-/opt/qwen-cluster}"
XI="$ROOT/bin/xi"
ENV_FILE=/etc/qwen-cluster.env

[ -x "$XI" ] || { echo "xi not found or not executable at $XI" >&2; exit 1; }
if [ ! -f "$ENV_FILE" ]; then
    umask 077
    echo 'QWEN_CLUSTER_TOKEN=' > "$ENV_FILE"
    echo "created $ENV_FILE: put the cluster token in it" >&2
fi

SETTINGS="$ROOT/xi.yaml"
if [ ! -f "$SETTINGS" ]; then
    cat > "$SETTINGS" <<EOF
# XI node agent settings (architecture/02-node-agent.md section 10). Also editable on the
# agent web page. The cluster token is NOT stored here (use QWEN_CLUSTER_TOKEN).
coordinator: '$COORDINATOR'
llama_dir: '$ROOT/llama.cpp'
models_dir: '$ROOT/models'
log_dir: '$ROOT/logs'
exec_dir: '$ROOT/exec'
EOF
    echo "created $SETTINGS"
elif [ -n "$COORDINATOR" ]; then
    if grep -q '^coordinator:' "$SETTINGS"; then
        sed -i.bak "s|^coordinator:.*|coordinator: '$COORDINATOR'|" "$SETTINGS"
    else
        echo "coordinator: '$COORDINATOR'" >> "$SETTINGS"
    fi
    echo "set coordinator in $SETTINGS (other settings kept)"
fi

ARGS="--config $SETTINGS"
if [ "${XI_ALLOW_EXEC:-0}" = 1 ]; then
    echo "warning: console code execution is ON: the cluster token can run programs on this machine" >&2
    ARGS="$ARGS --allow-exec"
fi

case "$(uname -s)" in
Linux)
    cat > /etc/systemd/system/qwen-xi.service <<EOF
[Unit]
Description=Qwen Cluster node agent (XI)
After=network-online.target
Wants=network-online.target

[Service]
EnvironmentFile=$ENV_FILE
WorkingDirectory=$ROOT
ExecStart=$XI $ARGS
Restart=always
RestartSec=60
KillMode=mixed
TimeoutStopSec=40

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    systemctl enable --now qwen-xi.service
    echo "installed and started qwen-xi.service (journalctl -u qwen-xi -f)"
    ;;
Darwin)
    TOKEN="$(sed -n 's/^QWEN_CLUSTER_TOKEN=//p' "$ENV_FILE")"
    PLIST=/Library/LaunchDaemons/org.qwen-cluster.xi.plist
    PROGRAM_ARGS="<string>$XI</string>"
    for a in $ARGS; do PROGRAM_ARGS="$PROGRAM_ARGS<string>$a</string>"; done
    cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>org.qwen-cluster.xi</string>
  <key>ProgramArguments</key><array>$PROGRAM_ARGS</array>
  <key>WorkingDirectory</key><string>$ROOT</string>
  <key>EnvironmentVariables</key><dict><key>QWEN_CLUSTER_TOKEN</key><string>$TOKEN</string></dict>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
</dict></plist>
EOF
    chmod 600 "$PLIST"
    launchctl bootout system "$PLIST" 2>/dev/null || true
    launchctl bootstrap system "$PLIST"
    echo "installed and started $PLIST"
    ;;
*)
    echo "unsupported OS $(uname -s)" >&2
    exit 1
    ;;
esac
