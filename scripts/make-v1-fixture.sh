#!/bin/sh
# Copy the bundled genuine schema-v1 fixture; no old Git objects or network needed.
# These credentials and identities are synthetic PUBLIC TEST DATA, never for deployment.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:?usage: make-v1-fixture.sh OUT_DIR}
fixture="$here/tests/integration/fixtures/v1"
(cd "$fixture" && sha256sum -c SHA256SUMS)
mkdir -p "$out"
cp "$fixture/v1-state.tar" "$fixture/v1-snapshot.json" "$fixture/v1-version.txt" "$fixture/SHA256SUMS" "$fixture/README.md" "$out/"
echo "copied genuine synthetic schema-v1 fixture to $out"
