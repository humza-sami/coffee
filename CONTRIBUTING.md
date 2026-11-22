# Contributing to Coffee

Coffee is a beta, native C++20 runtime. Contributions that keep its claims honest are the most valuable ones.

## Ground rules

* **Claims need evidence.** Do not describe pooled memory, transparent threads, durable pooled disk, HA, encryption or multi-device
  performance unless a test or measurement in the repository shows it. Say "local-process simulation" when that is what it is.
* **Write the regression first.** For a bug, add a test that fails before the fix and passes after; do not weaken an existing test to make
  a change pass. Failure and fault paths (kill, timeout, cancel, restart, corrupt data) deserve real-process tests, not only mocks.
* **Keep modules focused** and the runtime free of AI services and network dependencies other than the coordinator protocol.
* **No secrets.** Never commit tokens, `STATE/token` files, personal machine state, logs with lease ids, or downloaded papers without
  redistribution rights.

## Build and test

```sh
scripts/bootstrap.sh && export PATH="$PWD/.tools/venv/bin:$PATH"
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure                 # unit, CLI, protocol, cluster, faults, demo, hardening, quickstart, upgrade, install and install safety
# separate sanitizer build (do not benchmark with it):
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

The genuine schema-v1 upgrade test runs by default from a [bundled synthetic fixture](tests/integration/fixtures/v1/README.md); no historical Git objects are needed. The fixture token is public test data, never a deployment credential.
Benchmarks: [docs/benchmarking.md](docs/benchmarking.md). Python is used only for test/benchmark tooling.

## Pull requests

Describe the behaviour change, the test that proves it, commands you ran and their results, and any documentation updated. Keep
unrelated reformatting out. By contributing you agree your work is licensed under Apache-2.0 ([LICENSE](LICENSE)). Be respectful and
constructive in issues and reviews.

## Security issues

Do not open a public issue; see [SECURITY.md](SECURITY.md).
