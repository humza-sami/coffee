#!/usr/bin/env bash
# Local-process demonstration of the cluster behaviour with assertions. LOCAL SIMULATION: the coordinator and both Beans
# are processes on THIS machine talking over loopback; it says nothing about multi-machine performance or networks.
# Usage: scripts/demo-local.sh [path/to/coffee]   (defaults to build/coffee). Uses a temp dir and an ephemeral port.
# Exit status: 0 only if EVERY required step and check passed (the last line is then "DEMO PASSED"); 1 otherwise, with a
# "DEMO FAILED: <reason>" line on stderr. Processes and the temp dir are always cleaned up.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COFFEE="${1:-$ROOT/build/coffee}"
T="$(mktemp -d /tmp/coffee-demo-XXXXXX)"
PIDS=()
cleanup() {
  local rc=$?
  trap - EXIT
  for p in "${PIDS[@]:-}"; do [[ -n "$p" ]] && kill -KILL "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -rf "$T"
  exit "$rc"
}
trap cleanup EXIT
say() { printf '\n=== %s\n' "$*"; }
fail() { echo "DEMO FAILED: $*" >&2; exit 1; }
expect_eq() { [[ "$2" == "$3" ]] || fail "$1: expected '$3' but got '$2'"; }
alive() { kill -0 "$1" 2>/dev/null; }
wait_for() { # description timeout_seconds command...
  local what="$1" n=$(( $2 * 10 )); shift 2
  for _ in $(seq "$n"); do "$@" && return 0; sleep 0.1; done
  fail "timed out waiting for $what"
}

PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')" || fail "no free port"
URL="http://127.0.0.1:$PORT"
echo "LOCAL SIMULATION on $(hostname) ($(uname -sr)); coordinator $URL; temp dir $T"
"$COFFEE" --version || fail "coffee binary does not run: $COFFEE"
"$COFFEE" init --state "$T/state" || fail "init"
C=(--server "$URL" --token-file "$T/state/token")

say "start coordinator (2 s leases) and two Beans (1 CPU each)"
"$COFFEE" serve --state "$T/state" --listen "127.0.0.1:$PORT" --lease-ms 2000 --heartbeat-timeout-ms 2000 --ready-file "$T/ready.json" >"$T/serve.log" 2>&1 & PIDS+=($!)
SERVE_PID=$!
wait_for "coordinator ready file" 15 test -f "$T/ready.json"
alive "$SERVE_PID" || fail "coordinator exited: $(cat "$T/serve.log")"
cat "$T/ready.json"
declare -A BEAN_PID
for n in bean-a bean-b; do
  "$COFFEE" join "${C[@]}" --work-dir "$T/work-$n" --id $n --cpus 1 --memory-mb 512 --heartbeat-ms 300 --poll-ms 100 >"$T/$n.log" 2>&1 & PIDS+=($!)
  BEAN_PID[$n]=$!
done
beans_up() { [[ "$("$COFFEE" beans "${C[@]}" --json 2>/dev/null | python3 -c 'import json,sys;print(sum(1 for b in json.load(sys.stdin)["beans"] if b["state"]=="online"))' 2>/dev/null)" == 2 ]]; }
wait_for "two online Beans" 15 beans_up
"$COFFEE" beans "${C[@]}"

say "1. file processing: examples/coffee.yaml, verified input/output transfer, results written to $T/out1"
"$COFFEE" brew "$ROOT/examples/coffee.yaml" "${C[@]}" --job-id wc1 --wait --out "$T/out1" || fail "brew of examples/coffee.yaml did not succeed"
echo "--- outputs"; for f in "$T"/out1/*/*; do echo "$f: $(tr '\n' ' ' <"$f")"; done
expect_eq "count-1 word count" "$(tr -d ' \n' <"$T/out1/count-1/count.txt")" "$(wc -w <"$ROOT/examples/data/part-1.txt" | tr -d ' ')"
expect_eq "count-2 word count" "$(tr -d ' \n' <"$T/out1/count-2/count.txt")" "$(wc -w <"$ROOT/examples/data/part-2.txt" | tr -d ' ')"
expect_eq "summary" "$(cat "$T/out1/summary/summary.txt")" "summary-complete"

say "2. two-step dependency-output pipeline: examples/pipeline/coffee.yaml"
"$COFFEE" brew "$ROOT/examples/pipeline/coffee.yaml" "${C[@]}" --job-id pipe1 --wait --out "$T/out2" || fail "brew of examples/pipeline/coffee.yaml did not succeed"
echo "--- stats/size.txt:"; cat "$T/out2/stats/size.txt"
IN="$ROOT/examples/pipeline/data/in.txt"
expect_eq "pipeline stats output" "$(cat "$T/out2/stats/size.txt")" "$(wc -c <"$IN" | tr -d ' ')"$'\n'"$(tr a-z A-Z <"$IN")"

say "3. unauthorized call and malformed JSON are controlled errors"
CODE="$(curl -s -o /dev/null -w '%{http_code}' "$URL/v1/jobs")" || fail "curl failed"
echo "GET /v1/jobs without token -> HTTP $CODE"; expect_eq "unauthorized status" "$CODE" 401
CODE="$(curl -s -o "$T/bad.json" -w '%{http_code}' -H "Authorization: Bearer $(cat "$T/state/token")" -H 'Content-Type: application/json' -d '{not json' "$URL/v1/beans")" || fail "curl failed"
echo "malformed JSON -> HTTP $CODE $(cat "$T/bad.json")"; expect_eq "malformed JSON status" "$CODE" 400

say "4. kill -9 the Bean running a task that has an ordinary child process; the survivor retries"
cat >"$T/kill.yaml" <<YAML
apiVersion: coffee/v1alpha1
kind: Workload
metadata: {name: kill-demo}
spec:
  tasks:
    - id: victim
      command: ["sh", "-c", "echo \$\$ > $T/shell.pid; sleep 300 & echo \$! > $T/child.pid; wait; echo finished > res.txt"]
      outputs: [res.txt]
      max_attempts: 3
YAML
"$COFFEE" brew "$T/kill.yaml" "${C[@]}" --job-id kill1 || fail "submitting the kill workload"
wait_for "the victim command to start" 20 test -s "$T/child.pid"
SH="$(cat "$T/shell.pid")"; CH="$(cat "$T/child.pid")"
HOLDER="$("$COFFEE" status kill1 "${C[@]}" --json | python3 -c 'import json,sys;print(json.load(sys.stdin)["tasks"][0]["attempts"][-1]["bean_id"])')" || fail "status"
HPID="${BEAN_PID[$HOLDER]:-}"; [[ -n "$HPID" ]] || fail "unknown holder '$HOLDER'"
echo "task runs on $HOLDER (Bean pid $HPID); command shell pid $SH, its ordinary child sleep pid $CH"
alive "$SH" && alive "$CH" || fail "command processes were not alive before the kill"
kill -KILL "$HPID"; echo "sent SIGKILL to Bean $HOLDER"
gone() { ! alive "$SH" && ! alive "$CH"; }
wait_for "command and ordinary child to stop after the Bean was killed" 10 gone
echo "pid $SH stopped; pid $CH stopped"
rm -f "$T/child.pid" "$T/shell.pid"
wait_for "the retry to start on the survivor" 20 test -s "$T/child.pid"
echo "retry started on the survivor; releasing it (kill its sleep child so 'wait' returns)"
kill "$(cat "$T/child.pid")"
"$COFFEE" brew "$T/kill.yaml" "${C[@]}" --job-id kill1 --wait --timeout 60 || fail "the retried job did not succeed"
echo "--- attempt history (note two distinct Beans):"; "$COFFEE" status kill1 "${C[@]}"
HIST="$("$COFFEE" status kill1 "${C[@]}" --json | python3 -c '
import json,sys
a=json.load(sys.stdin)["tasks"][0]["attempts"]
print(len(a), a[0]["state"], a[-1]["state"], a[0]["bean_id"]!=a[-1]["bean_id"])')" || fail "status"
expect_eq "attempt history (count, first state, last state, distinct Beans)" "$HIST" "2 expired succeeded True"

say "5. job list (earlier results are untouched by the retry)"
"$COFFEE" status "${C[@]}"
JOBS="$("$COFFEE" status "${C[@]}" --json | python3 -c 'import json,sys;print(" ".join(sorted(j["id"]+"="+j["state"] for j in json.load(sys.stdin)["jobs"])))')" || fail "status"
expect_eq "final job states" "$JOBS" "kill1=succeeded pipe1=succeeded wc1=succeeded"
echo; echo "DEMO PASSED: all checks held; cleaning up processes and $T"
