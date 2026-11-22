# Verification for Coffee 0.1.0-alpha.1

The release is exercised as local processes on Ubuntu 26.04.1, kernel 7.0.0-43, GCC 15.2.0, CMake 4.3.4, Ninja 1.13.2 and Python 3.14.4. These checks do not establish physical multi-device, WAN, power-loss or other-distribution compatibility. Hosted CI is configured but has not been executed.

## Reproduce

```sh
scripts/bootstrap.sh
export PATH="$PWD/.tools/venv/bin:$PATH"
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

CTest has 11 suites: unit, CLI smoke, protocol, cluster, faults, demo, hardening, quickstart, upgrade, install and install safety. Installation checks exercise a temporary prefix, collisions, edited-file preservation, uninstall, the wrapper and CPack. Tests start real coordinator/Bean processes on loopback with temporary state.

The [bundled fixture](../../tests/integration/fixtures/v1/README.md) was produced by the genuine schema-v1 implementation. Its checksummed synthetic state migrates to schema 2 while preserving cluster identity, historical tasks/attempts/results, configuration and token; the test executes new work, checks its actual output and restarts the coordinator. Missing or corrupt selected fixture data fails explicitly. No historical Git objects or private source files are needed.

Separate ASAN+UBSAN builds are for correctness only; see [build instructions](../native-build.md). The migration check includes fixture-integrity negative cases as well as the real-process upgrade. Sanitizer instrumentation is never used for benchmark timing.

## Results and limits

The earlier fresh source export without Git history or private development files passed all 11 release CTest suites using the pinned dependency cache. Its focused schema-v1 migration suite passed under ASAN+UBSAN; missing and damaged fixture selections failed explicitly.

After the temporary-transfer lifecycle corrections, the final source passed all 11 suites in both Release and ASAN+UBSAN builds, with the default bundled migration fixture and no skips. Each full run includes 102 unit cases / 860 assertions, 32 protocol tests, 14 fault tests and all three upgrade tests. Focused runs additionally passed 60 artifact assertions across nine cases, nine HTTP lifecycle tests and three slow-publication/race tests in both builds. They cover input-scope cleanup, cancelled/expired/abandoned output reclamation, valid staged completion across restart, concurrent inputs, interrupted transfers and cleanup deferred through active completion; renewals remain live during slow publication. These are the author's local checks; independent review remains a separate gate. This is not hosted CI or cross-platform verification.

The [benchmark](../benchmarking.md) retains 42 measured/warm-up runs with identical verified outputs and reports 30–53% overhead versus equal-budget pinned direct processes for its short-task workload. Historical internal source identifiers in the measured evidence are archival identifiers, not commits available in public history. Source-content checksums are included; a contemporaneous binary digest was not recorded. No measurements were rerun for subsequent source or documentation changes, including temporary-transfer cleanup; the archived benchmark does not measure the final release executable.

Observed binary compatibility on the original build host: dynamic links to libstdc++, libgcc_s, libc and libm; highest required symbol versions `GLIBC_2.38`, `GLIBCXX_3.4.32`, `CXXABI_1.3.15` (host glibc 2.43). This does not establish portability to older distributions.
