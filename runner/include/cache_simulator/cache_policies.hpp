#pragma once
#include "cache_simulator/cache_hierarchy.hpp"
#include <nlohmann/json.hpp>

namespace cache_simulator {
// Optional JSON policies; throws invalid_argument for unknown fields/values.
void configure_policies(CacheConfig &config, const nlohmann::json &level);
HierarchyConfig parse_hierarchy_policies(const nlohmann::json &value,
                                         std::size_t levels);
nlohmann::json describe_policies(const HierarchyConfig &policies,
                                 const std::vector<CacheConfig> &levels);
nlohmann::json describe_policies(const CacheHierarchy &hierarchy);
} // namespace cache_simulator
