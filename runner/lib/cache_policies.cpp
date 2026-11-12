#include "cache_simulator/cache_policies.hpp"
#include <stdexcept>

namespace cache_simulator {
using Json = nlohmann::json;
void configure_policies(CacheConfig &config, const Json &level) {
  auto write = level.value("write_policy", Json("write_back"));
  if (write == "write_back")
    config.write_policy = WritePolicy::WriteBack;
  else if (write == "write_through")
    config.write_policy = WritePolicy::WriteThrough;
  else
    throw std::invalid_argument(
        "write_policy must be write_back or write_through");
  auto allocation = level.value("allocation_policy", Json("write_allocate"));
  if (allocation == "write_allocate")
    config.allocation_policy = AllocationPolicy::WriteAllocate;
  else if (allocation == "no_write_allocate")
    config.allocation_policy = AllocationPolicy::NoWriteAllocate;
  else
    throw std::invalid_argument(
        "allocation_policy must be write_allocate or no_write_allocate");
}
HierarchyConfig parse_hierarchy_policies(const Json &value,
                                         std::size_t levels) {
  if (!levels || !value.is_object())
    throw std::invalid_argument(
        "hierarchy must be an object with at least one cache level");
  for (const auto &entry : value.items())
    if (entry.key() != "inclusion_policies" &&
        entry.key() != "writeback_clean" && entry.key() != "non_inclusive_fill")
      throw std::invalid_argument("unknown hierarchy configuration field: " +
                                  entry.key());
  HierarchyConfig config;
  if (value.contains("inclusion_policies")) {
    const auto &links = value.at("inclusion_policies");
    if (!links.is_array() || links.size() != levels - 1)
      throw std::invalid_argument(
          "provide one inclusion policy per adjacent pair");
    for (const auto &link : links) {
      if (link == "exclusive")
        config.inclusion_policies.push_back(InclusionPolicy::Exclusive);
      else if (link == "inclusive")
        config.inclusion_policies.push_back(InclusionPolicy::Inclusive);
      else if (link == "non_inclusive")
        config.inclusion_policies.push_back(InclusionPolicy::NonInclusive);
      else
        throw std::invalid_argument("unknown inclusion policy");
    }
  } else
    config.inclusion_policies.assign(levels - 1, InclusionPolicy::Exclusive);
  if (value.contains("writeback_clean")) {
    if (!value.at("writeback_clean").is_boolean())
      throw std::invalid_argument("writeback_clean must be boolean");
    config.writeback_clean = value.at("writeback_clean").get<bool>();
  }
  const auto fill = value.value("non_inclusive_fill", Json("all_levels"));
  if (fill != "all_levels" && fill != "on_eviction")
    throw std::invalid_argument(
        "non_inclusive_fill must be all_levels or on_eviction");
  config.non_inclusive_fill = fill == "all_levels";
  return config;
}
Json describe_policies(const HierarchyConfig &policies,
                       const std::vector<CacheConfig> &levels) {
  Json links = Json::array();
  std::string inclusion = "exclusive";
  for (auto policy : policies.inclusion_policies) {
    const std::string name = to_string(policy);
    if (links.empty())
      inclusion = name;
    else if (inclusion != name)
      inclusion = "mixed";
    links.push_back(name);
  }
  std::string write = to_string(levels.at(0).write_policy);
  std::string allocation = to_string(levels.at(0).allocation_policy);
  const auto count = levels.size();
  for (std::size_t level = 2; level <= count; ++level) {
    if (write != to_string(levels[level - 1].write_policy))
      write = "mixed";
    if (allocation != to_string(levels[level - 1].allocation_policy))
      allocation = "mixed";
  }
  return {{"inclusion_policy", inclusion},
          {"write_policy", write},
          {"allocation_policy", allocation},
          {"inclusion_policies", links},
          {"writeback_clean", policies.writeback_clean},
          {"non_inclusive_fill",
           policies.non_inclusive_fill ? "all_levels" : "on_eviction"}};
}
Json describe_policies(const CacheHierarchy &hierarchy) {
  std::vector<CacheConfig> levels;
  const auto count = hierarchy.level_names().size();
  for (std::size_t i = 1; i <= count; ++i)
    levels.push_back(hierarchy.configuration(i));
  return describe_policies(hierarchy.policies(), levels);
}
} // namespace cache_simulator
