// Coffee command-line front end. main() only forwards to run_cli so the CLI is unit-testable.
//
// Exit codes: 0 success; 1 operation failed (invalid workload, init failure, I/O);
//             2 usage error (unknown command/option, missing argument);
//             3 command recognised but not implemented in this build;
//             4 coordinator unreachable, unauthorized or server error; 5 `brew --wait` timed out.
#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace coffee {

// `args` excludes argv[0]. Normal output goes to `out`, diagnostics to `err`.
int run_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);

}  // namespace coffee
