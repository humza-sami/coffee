// Internal: one claimed attempt (stage inputs -> execute -> publish outputs -> complete).
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "coffee/client.hpp"
#include "coffee/worker.hpp"

namespace coffee::detail {

struct AttemptContext {
  const WorkerOptions* options = nullptr;
  Client* client = nullptr;
  std::string bean_id;
  const std::atomic<bool>* stopping = nullptr;
  std::vector<int> cpu_ids;                 // CPUs assigned to this attempt
  std::function<void(const std::string&)> log;
};

// Runs the claim to completion; never throws.
void run_attempt(const AttemptContext& ctx, const nlohmann::json& claim);

}  // namespace coffee::detail
