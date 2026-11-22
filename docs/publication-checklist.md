# Release checklist

- [ ] Verify version consistency in `include/coffee/util.hpp`, `CMakeLists.txt`, the README, release notes and `coffee --version`. Mark alpha/beta tags as prereleases.
- [ ] Build and test the exact release source: all 11 CTest suites, including bundled schema-v1 migration, installation and uninstall safety. Run a separate ASAN/UBSAN build for correctness.
- [ ] Check that a source archive without Git history builds and tests. Verify bundled fixture hashes; keep its synthetic credentials out of real deployments.
- [ ] Confirm README commands and relative links work; compare performance statements with raw measurements and disclose unsupported environments.
- [ ] State hosted CI status truthfully; local verification is not a hosted CI result.
- [ ] Include source, tests, examples, packaging, product documentation and public measured evidence. Exclude credentials, personal machine state, build outputs and private development records.
- [ ] Retain `LICENSE`, `NOTICE` and all third-party licenses. Credit generated artwork accurately.
- [ ] Build the release tarball from the verified revision and record its SHA-256. Publish the tag and artifact only after review; keep incomplete work local and avoid force pushes.
