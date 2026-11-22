#!/usr/bin/env bash
# Native CLI smoke test: coffee <binary> <example.yaml>
set -euo pipefail
BIN="$1"; EXAMPLE="$2"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

"$BIN" --version | grep -q '^coffee ' || fail "version"
"$BIN" --help | grep -q 'validate' || fail "help"
"$BIN" init --state "$TMP/state" >/dev/null || fail "init"
[[ "$(stat -c %a "$TMP/state/token")" == "600" ]] || fail "token mode"
ID1="$(cat "$TMP/state/config.json")"; TOK1="$(cat "$TMP/state/token")"
"$BIN" init --state "$TMP/state" >/dev/null || fail "re-init"
[[ "$ID1" == "$(cat "$TMP/state/config.json")" && "$TOK1" == "$(cat "$TMP/state/token")" ]] || fail "re-init overwrote identity"
"$BIN" validate "$EXAMPLE" --json | grep -q '"valid":true' || fail "validate example"
printf 'apiVersion: coffee/v1alpha1\nkind: Workload\n' > "$TMP/bad.yaml"
if "$BIN" validate "$TMP/bad.yaml" 2>/dev/null; then fail "bad workload accepted"; fi
"$BIN" resources --path "$TMP" --json | grep -q '"schema_version":1' || fail "resources json"
"$BIN" resources --path "$TMP" | grep -q 'no resources were allocated' || fail "resources human"
set +e
"$BIN" resources --path "$TMP/missing" --json | grep -q '"code":"io_error"'; [[ ${PIPESTATUS[0]} -eq 1 ]] || fail "resources missing path"
"$BIN" resources --cpus 0 2>/dev/null; [[ $? -eq 2 ]] || fail "resources bad cpus exit code"
"$BIN" bogus 2>/dev/null; [[ $? -eq 2 ]] || fail "unknown command exit code"
"$BIN" prepare 2>/dev/null; [[ $? -eq 3 ]] || fail "unimplemented exit code"
echo "cli smoke OK"
