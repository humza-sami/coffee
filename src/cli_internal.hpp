// Internal: network-facing CLI commands (serve, join, beans, plan, brew, status, logs, explain, cancel, drain).
#pragma once

#include <iosfwd>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace coffee::cli {

inline constexpr int kOk = 0, kFailed = 1, kUsage = 2, kUnimplemented = 3, kNetwork = 4, kTimeout = 5;

struct Options {
  std::map<std::string, std::string> values;  // --name VALUE / --name=VALUE
  std::set<std::string> flags;                // --name
  std::vector<std::string> positional;
  std::string error;
  bool has(const std::string& k) const { return values.count(k) || flags.count(k); }
  std::optional<std::string> get(const std::string& k) const {
    auto it = values.find(k);
    if (it == values.end()) return std::nullopt;
    return it->second;
  }
};

// Parses `args` against the allowed value-taking and boolean options (plus --help/-h which sets flag "help").
Options parse_options(const std::vector<std::string>& args, const std::set<std::string>& value_opts, const std::set<std::string>& flag_opts);

// Returns true when `cmd` is one of the network commands and stores its exit status in `status`.
bool run_net_command(const std::string& cmd, const std::vector<std::string>& args, std::ostream& out, std::ostream& err, int& status);

}  // namespace coffee::cli
