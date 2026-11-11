#pragma once

#include "cache_simulator/SetAssociativeCacheLevel.h"
#include "cache_simulator/cache_hierarchy.hpp"
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace cache_simulator {

struct ApiResponse {
  int status = 200;
  nlohmann::json body;
  std::string location;
};

// The transport calls this concurrently. Execution publishes whole batches;
// preparation uses one cancellable background worker.
class RunnerApi {
public:
  using LevelFactory = std::function<std::unique_ptr<ICacheLevel>(
      const CacheConfig &, const ReplacementPolicyFactory &)>;
  explicit RunnerApi(LevelFactory factory = {});
  ~RunnerApi();
  ApiResponse handle(const std::string &method, const std::string &path,
                     const std::multimap<std::string, std::string> &query,
                     const nlohmann::json &body);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cache_simulator
