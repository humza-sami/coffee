// Coffee workload model: YAML parsing, validation and normalization (schema coffee/v1alpha1).
//
// Normalized representation (also the accepted JSON form; normalize_workload is idempotent):
//   {
//     "apiVersion": "coffee/v1alpha1", "kind": "Workload",
//     "metadata": {"name": "<name>"},
//     "spec": {"tasks": [{
//        "id": "<id>", "command": ["argv0", ...], "depends_on": ["<id>", ...],
//        "inputs": [{"source": "rel/path/in/workload/dir", "target": "rel/path/in/attempt/dir"},
//                   {"from_task": "<producer id>", "output": "<declared producer output>", "target": "rel/path"}],
//        "outputs": ["rel/path", ...],
//        "resources": {"cpus": 1, "memory_mb": 256},
//        "timeout_seconds": 300, "max_attempts": 3 }]}
//   }
// Names/ids match [A-Za-z0-9][A-Za-z0-9._-]{0,62}. Unknown fields are rejected.
// An input has exactly one of `source` (local file) or `from_task`+`output` (dependency output). The producer
// must be in the consumer's explicit depends_on and `output` must be one of its declared outputs; targets obey
// the same safe-path and conflict rules for both kinds.
#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "coffee/util.hpp"

namespace coffee {

inline constexpr size_t kMaxWorkloadBytes = 1 << 20;  // 1 MiB of YAML
inline constexpr size_t kMaxTasks = 4096;

// Controlled validation diagnostic. field() is a path such as "spec.tasks[2].resources.cpus".
class WorkloadError : public Error {
 public:
  WorkloadError(std::string field, const std::string& message)
      : Error("invalid_workload", field.empty() ? message : field + ": " + message), field_(std::move(field)), detail_(message) {}
  const std::string& field() const noexcept { return field_; }
  const std::string& detail() const noexcept { return detail_; }  // message without the field prefix

 private:
  std::string field_;
  std::string detail_;
};

struct NormalizeOptions {
  // When set, every input `source` must be an existing regular file below this directory
  // (symlinks that resolve outside it are rejected).
  std::optional<std::filesystem::path> source_root;
};

// Read, parse and normalize a workload file. Input sources are resolved relative to the file's
// directory and must exist. Throws WorkloadError (schema/semantic problem) or Error (I/O).
nlohmann::json parse_workload(const std::filesystem::path& path);

// Parse YAML text. `source_root` (if given) enables input-file checks as in NormalizeOptions.
nlohmann::json parse_workload_text(const std::string& yaml, const std::optional<std::filesystem::path>& source_root = std::nullopt);

// Validate + apply defaults to an already-decoded workload document (YAML->JSON or API JSON).
nlohmann::json normalize_workload(const nlohmann::json& doc, const NormalizeOptions& options = {});

}  // namespace coffee
