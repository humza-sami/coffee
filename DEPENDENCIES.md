# Dependencies and licenses

All sources are fetched by `scripts/bootstrap.sh` into the git-ignored `.tools/` directory
and verified against the SHA-256 values below. Nothing is installed globally.

| Component | Version | Use | License | Archive SHA-256 |
| --- | --- | --- | --- | --- |
| SQLite amalgamation | 3.54.1 | durable state store | Public domain ([notice](third_party/licenses/SQLite.NOTICE)) | `0a9c8e34352dad46006aa82836d5383bff9dc6bc9ce5a60553c05aa30e6ad1f9` |
| yaml-cpp | 0.9.0 | workload YAML parsing | MIT ([license](third_party/licenses/yaml-cpp.LICENSE)) | `25cb043240f828a8c51beb830569634bc7ac603978e0f69d6b63558dadefd49a` |
| nlohmann/json | 3.12.0 (`json.hpp`) | normalized workload / JSON output | MIT ([license](third_party/licenses/nlohmann-json.LICENSE)) | `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63` |
| Catch2 (amalgamated) | 3.16.1 | unit tests only | BSL-1.0 ([license](third_party/licenses/Catch2.LICENSE)) | `d16bd1b8b2364918bd472cb216394acb93bde6784890df0d91bccfb3f28d777d` |
| cpp-httplib | 0.67.0 (`httplib.h`, built without TLS/zlib/brotli) | coordinator HTTP server and client | MIT ([license](third_party/licenses/cpp-httplib.LICENSE)) | `be711ddaf7a3655d8f23aa5655709250b24d247aedd4b2629989d18b6164e5be` (tag tarball; `httplib.h` + `LICENSE` extracted) |
| CMake (PyPI wheel) | 4.3.4 | build tool | BSD-3-Clause and bundled licenses | version-pinned via pip |
| Ninja (PyPI wheel) | 1.13.2 | build tool | Apache-2.0 | version-pinned via pip |

OpenSSL is not used: no development headers exist on the target and none were vendored, so this build speaks plain HTTP only and
documents SSH/WireGuard/Tailscale for encrypted connectivity ([docs/native-protocol.md](docs/native-protocol.md)). Integration tests use the
system `python3` standard library only.
Network access is needed only for `scripts/bootstrap.sh` (sqlite.org, github.com, pypi.org).
