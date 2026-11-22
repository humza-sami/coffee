# Security policy

Coffee 0.1.0-beta.1 is a **beta** for a *trusted owner* running their own machines. Read [limitations](docs/limitations.md) first:
there is no TLS, one shared bearer token, no sandbox and no multi-tenant isolation. Those are known design limits, not vulnerabilities to
report, but bugs that break the documented guarantees are.

## Reporting a vulnerability

Please report privately and do **not** open a public issue with exploit details. Use GitHub's "Report a vulnerability" (private security
advisory) on this repository once it is published, or contact the maintainers by the private address listed on the repository profile.
Include the version (`coffee --version`), a minimal reproduction, expected vs actual behaviour and impact. Never include a real token.
You can expect an acknowledgement; there is no bounty and no service-level guarantee for this beta.

## In scope

Authentication bypass, path traversal/symlink escape from an attempt workspace or artifact store, acceptance of unverified or stale
artifacts, lease/fencing bypass, token disclosure in logs/argv/output, memory-safety bugs in the coordinator or Bean.

## Out of scope for this release

Attacks that need a malicious but authorized token holder or Bean (trusted-owner model), traffic sniffing on an unencrypted link you
chose to use, denial of service by a peer on a network you exposed the port to, and the unsupported items in the limitations document.
