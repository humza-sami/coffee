#!/bin/sh
# Install a built Coffee into a user-chosen prefix (no sudo, no system path default).
#   scripts/install.sh --prefix DIR [--build-dir build]
# Thin wrapper over `cmake --install`; refuses to overwrite anything Coffee did not install. Remove with
# DIR/share/coffee/uninstall.sh --prefix DIR.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
prefix=""; build="$here/build"
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) [ $# -ge 2 ] || { echo "install: --prefix needs a value" >&2; exit 2; }; prefix=$2; shift 2 ;;
    --build-dir) [ $# -ge 2 ] || { echo "install: --build-dir needs a value" >&2; exit 2; }; build=$2; shift 2 ;;
    -h|--help) echo "usage: install.sh --prefix DIR [--build-dir DIR]"; exit 0 ;;
    *) echo "install: unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$prefix" ] || { echo "install: --prefix DIR is required (there is no default and nothing is installed system-wide)" >&2; exit 2; }
cmake=$(command -v cmake || true)
[ -n "$cmake" ] || cmake="$here/.tools/venv/bin/cmake"
[ -x "$cmake" ] || { echo "install: cmake not found (run scripts/bootstrap.sh)" >&2; exit 1; }
[ -x "$build/coffee" ] || { echo "install: $build/coffee is not built (see docs/native-build.md)" >&2; exit 1; }
exec "$cmake" --install "$build" --prefix "$prefix"
