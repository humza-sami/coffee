#!/usr/bin/env bash
# Project-local bootstrap: installs CMake/Ninja into .tools/venv and fetches pinned,
# checksum-verified dependency sources into .tools/deps. Nothing is installed globally.
# Requires: bash, curl, tar, python3 (with venv), sha256sum, a C++20 compiler.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOLS="$ROOT/.tools"
DEPS="$TOOLS/deps"
CMAKE_VERSION="4.3.4"
NINJA_VERSION="1.13.2"

fetch() { # name url sha256 outfile
  local dest="$TOOLS/dl/$1"
  mkdir -p "$dest"
  if [[ ! -f "$dest/$4" ]]; then
    curl -fsSL --retry 3 -o "$dest/$4.part" "$2"
    mv "$dest/$4.part" "$dest/$4"
  fi
  echo "$3  $dest/$4" | sha256sum -c --quiet - || { echo "checksum mismatch for $1" >&2; rm -f "$dest/$4"; exit 1; }
}

mkdir -p "$TOOLS" "$DEPS"

if [[ ! -x "$TOOLS/venv/bin/cmake" ]]; then
  python3 -m venv "$TOOLS/venv"
  "$TOOLS/venv/bin/pip" install --quiet "cmake==$CMAKE_VERSION" "ninja==$NINJA_VERSION"
fi

# SQLite 3.54.1 amalgamation (public domain)
if [[ ! -f "$DEPS/sqlite/sqlite3.c" ]]; then
  fetch sqlite https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip \
    0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9 sqlite.zip
  rm -rf "$DEPS/sqlite" && mkdir -p "$DEPS/sqlite"
  python3 -I -c 'import sys,zipfile
z=zipfile.ZipFile(sys.argv[1])
for n in ("sqlite3.c","sqlite3.h"):
    open(sys.argv[2]+"/"+n,"wb").write(z.read("sqlite-amalgamation-3540100/"+n))' \
    "$TOOLS/dl/sqlite/sqlite.zip" "$DEPS/sqlite"
fi

# yaml-cpp 0.9.0 (MIT)
if [[ ! -f "$DEPS/yaml-cpp/CMakeLists.txt" ]]; then
  fetch yaml-cpp https://github.com/jbeder/yaml-cpp/archive/refs/tags/yaml-cpp-0.9.0.tar.gz \
    25cb043240f828a8c51beb830569634bc7ac603978e0f69d6b63558dadefd49a yaml-cpp.tar.gz
  rm -rf "$DEPS/yaml-cpp" && mkdir -p "$DEPS/yaml-cpp"
  tar -xzf "$TOOLS/dl/yaml-cpp/yaml-cpp.tar.gz" -C "$DEPS/yaml-cpp" --strip-components=1
fi

# nlohmann/json 3.12.0 single header (MIT)
if [[ ! -f "$DEPS/json/nlohmann/json.hpp" ]]; then
  fetch json https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp \
    aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63 json.hpp
  mkdir -p "$DEPS/json/nlohmann"
  cp "$TOOLS/dl/json/json.hpp" "$DEPS/json/nlohmann/json.hpp"
fi

# cpp-httplib 0.67.0 single header (MIT); built without OpenSSL, so plain HTTP only (see docs/native-protocol.md)
if [[ ! -f "$DEPS/httplib/httplib.h" ]]; then
  fetch httplib https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.67.0.tar.gz \
    be711ddaf7a3655d8f23aa5655709250b24d247aedd4b2629989d18b6164e5be httplib.tar.gz
  rm -rf "$DEPS/httplib" && mkdir -p "$DEPS/httplib"
  tar -xzf "$TOOLS/dl/httplib/httplib.tar.gz" -C "$DEPS/httplib" --strip-components=1 cpp-httplib-0.67.0/httplib.h cpp-httplib-0.67.0/LICENSE
fi

# Catch2 3.16.1 amalgamated sources (BSL-1.0), used for tests only
if [[ ! -f "$DEPS/catch2/catch_amalgamated.cpp" ]]; then
  fetch catch2 https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.1.tar.gz \
    d16bd1b8b2364918bd472cb216394acb93bde6784890df0d91bccfb3f28d777d catch2.tar.gz
  rm -rf "$DEPS/catch2" && mkdir -p "$DEPS/catch2"
  tar -xzf "$TOOLS/dl/catch2/catch2.tar.gz" -C "$DEPS/catch2" --strip-components=2 \
    Catch2-3.16.1/extras/catch_amalgamated.cpp Catch2-3.16.1/extras/catch_amalgamated.hpp
fi

echo "bootstrap complete: $TOOLS"
