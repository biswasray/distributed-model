#!/bin/sh
# Registers XI (xi) as a boot-time service on Linux (systemd) or macOS (launchd).
# The Linux/macOS counterpart of install-xi-task.ps1 (architecture/02-node-agent.md §8).
#
# Usage (as root):  sudo ./install-xi-service.sh http://192.168.1.10:8100 [/opt/qwen-cluster]
# The token is read from /etc/qwen-cluster.env (QWEN_CLUSTER_TOKEN=...), which this script
# creates with mode 600 if it is missing.
set -eu

COORDINATOR="${1:?usage: $0 <coordinator-url> [root-dir]}"
ROOT="${2:-/opt/qwen-cluster}"
XI="$ROOT/bin/xi"
ENV_FILE=/etc/qwen-cluster.env

[ -x "$XI" ] || { echo "xi not found or not executable at $XI" >&2; exit 1; }
if [ ! -f "$ENV_FILE" ]; then
    umask 077
    echo 'QWEN_CLUSTER_TOKEN=' > "$ENV_FILE"
    echo "created $ENV_FILE: put the cluster token in it" >&2
fi

ARGS="--coordinator $COORDINATOR --llama-dir $ROOT/llama.cpp --models-dir $ROOT/models --log-dir $ROOT/logs"

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
