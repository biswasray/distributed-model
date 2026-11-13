#!/bin/sh
# POSIX smoke test (Linux, macOS): xgi serve + 3 xi agents on 127.0.0.1 with the mock llama.cpp
# binaries. Needs only sh and curl. The full suite is run-e2e.ps1 (PowerShell 7 on any OS).
#
# Usage: smoke.sh <xgi> <xi> <mock-llama-server> <mock-rpc-server>
set -u
XGI="$1"; XI="$2"; MOCK_LLAMA="$3"; MOCK_RPC="$4"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/xgi-smoke.XXXXXX")"
TOKEN=smoke-token
COORD=http://127.0.0.1:19100
ROUTER=http://127.0.0.1:19000
AGENT_A=http://127.0.0.1:19201
FAILS=0
PIDS=""

# ---- helpers
check() {  # check <description> <command...>
    desc="$1"; shift
    if "$@"; then echo "  PASS  $desc"; else echo "  FAIL  $desc"; FAILS=$((FAILS + 1)); fi
}
wait_for() {  # wait_for <seconds> <command...>
    n=$(($1 * 4)); shift
    while [ "$n" -gt 0 ]; do
        "$@" && return 0
        sleep 0.25; n=$((n - 1))
    done
    return 1
}
get() { curl -s -H "X-Cluster-Token: $TOKEN" "$1"; }
post() { curl -s -X POST -H "X-Cluster-Token: $TOKEN" -H "Content-Type: application/json" "$1"; }
chat() { curl -s -N -X POST -H "Content-Type: application/json" -d "$1" "$ROUTER/v1/chat/completions"; }
# One group's JSON on a single line (group objects contain no nested objects).
group() { get "$COORD/groups" | tr -d ' \n' | sed 's/},{/}\
{/g' | grep "\"group_id\":\"$1\""; }
pair_field() { group pair-0 | grep -q "$1"; }

alive_count_is() { [ "$(get "$COORD/nodes" | grep -o '"status":"alive"' | wc -l | tr -d ' ')" -eq "$1" ]; }
unauthorized() { [ "$(curl -s -o /dev/null -w '%{http_code}' "$COORD/nodes")" = 401 ]; }
deployed() { post "$COORD/deploy" | grep -q '"pair-0"'; }
healthy() { pair_field '"status":"healthy"'; }
not_healthy() { ! healthy; }
head_is_a() { pair_field '"head":"node-a"'; }
models_listed() { curl -s "$ROUTER/v1/models" | grep -q '"pair"'; }
completion_ok() { chat '{"model":"pair","messages":[]}' | grep -q 'Hello from mock'; }
stream_ok() { chat '{"model":"pair","stream":true,"messages":[]}' | grep -q '\[DONE\]'; }
repaired() { healthy && ! pair_field "\"workers\":\[\"$WORKER\"\]"; }
exit_seen() { pair_field 'exited'; }
undeployed() { post "$COORD/undeploy" | grep -q '"stopped":1'; }
head_stopped() { get "$AGENT_A/status" | tr -d ' \n' | grep -q '"head":{"exit_code":null,"pid":null,"state":"stopped"'; }

cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    sleep 1
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    pkill -f "^$MOCK_LLAMA" 2>/dev/null  # anchored: must not match this script's own command line
    pkill -f "^$MOCK_RPC" 2>/dev/null
    if [ "$FAILS" -eq 0 ]; then rm -rf "$WORK"; else echo "logs kept in $WORK"; fi
}
trap cleanup EXIT

# ---- cluster: node-a holds the model; node-b and node-c don't.
mkdir -p "$WORK/models-a" "$WORK/models-b" "$WORK/models-c"
echo GGUF > "$WORK/models-a/mock.gguf"
cat > "$WORK/cluster.yaml" <<EOF
cluster_token: "$TOKEN"
coordinator: {host: 127.0.0.1, port: 19100, heartbeat_interval_s: 1, node_timeout_s: 5, group_start_timeout_s: 60}
router: {host: 127.0.0.1, port: 19000, coordinator_url: "$COORD", request_timeout_s: 60}
pools:
  - {name: pair, model_file: mock.gguf, nodes_per_group: 2, groups: 1, extra_args: ["--mock-load-s", "1"]}
spares: 1
EOF

"$XGI" serve --config "$WORK/cluster.yaml" --log-dir "$WORK/logs-xgi" > "$WORK/xgi.out" 2>&1 &
PIDS="$PIDS $!"
i=0
for n in a b c; do
    i=$((i + 1))
    "$XI" --coordinator "$COORD" --token "$TOKEN" --name "node-$n" --host 127.0.0.1 --advertise-ip 127.0.0.1 \
        --port $((19200 + i)) --rpc-port $((50260 + i)) --head-port $((19080 + i)) --heartbeat-s 1 \
        --models-dir "$WORK/models-$n" --log-dir "$WORK/logs-$n" \
        --rpc-server-bin "$MOCK_RPC" --llama-server-bin "$MOCK_LLAMA" > "$WORK/xi-$n.out" 2>&1 &
    eval "XI_PID_$n=$!"
    PIDS="$PIDS $!"
done

check '3 agents registered' wait_for 20 alive_count_is 3
check 'coordinator requires X-Cluster-Token' unauthorized
check 'POST /deploy' deployed
check 'pair-0 becomes healthy' wait_for 30 healthy
check 'head is the node holding the model' head_is_a
check 'router lists the pool' wait_for 10 models_listed
check 'non-streaming completion' completion_ok
check 'streaming completion (SSE)' stream_ok

# Worker failure: kill the worker's agent (on Linux its rpc-server dies with it).
WORKER=$(group pair-0 | sed 's/.*"workers":\["\([^"]*\)".*/\1/')
eval "VICTIM=\$XI_PID_${WORKER#node-}"
echo "  ..    killing agent $WORKER (pid $VICTIM)"
kill -9 "$VICTIM"
check "group leaves healthy after $WORKER dies" wait_for 20 not_healthy
check 'group repaired onto the spare' wait_for 40 repaired

# Head process exit while its agent stays up.
HEAD_PID=$(get "$AGENT_A/status" | tr -d ' \n' | sed 's/.*"head":{[^}]*"pid":\([0-9]*\).*/\1/')
echo "  ..    killing llama-server pid $HEAD_PID"
kill -9 "$HEAD_PID"
check 'head process exit detected via heartbeat' wait_for 15 exit_seen
check 'group healthy again after the head restart' wait_for 30 healthy

check 'POST /undeploy' undeployed
check 'head process stopped on undeploy' head_stopped

if [ "$FAILS" -gt 0 ]; then echo "$FAILS check(s) FAILED"; exit 1; fi
echo "smoke: all checks passed"
