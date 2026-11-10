#include "cache_simulator/cache_hierarchy.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cache_simulator {

static void check_slot(const CacheConfig &config, std::size_t set,
                       std::size_t way) {
  const auto sets =
      config.size_bytes / config.line_size_bytes / config.associativity;
  if (set >= sets || way >= config.associativity)
    throw std::runtime_error("cache level returned invalid set/way");
}

CacheHierarchy::CacheHierarchy(std::vector<CacheLevel> levels)
    : levels_(std::move(levels)) {
  if (levels_.empty())
    throw std::runtime_error("hierarchy must contain at least one level");
  for (const auto &level : levels_) {
    const auto &config = level.config;
    if (!level.implementation || level.name.empty() || config.size_bytes == 0 ||
        config.line_size_bytes == 0 || config.associativity == 0 ||
        config.replacement_policy.empty() ||
        config.size_bytes % config.line_size_bytes != 0 ||
        (config.size_bytes / config.line_size_bytes) % config.associativity !=
            0)
      throw std::runtime_error("invalid cache level configuration");
    if (config.line_size_bytes != levels_.front().config.line_size_bytes)
      throw std::runtime_error("exclusive hierarchy requires equal line sizes");
  }
}

void CacheHierarchy::clear() {
  for (auto &level : levels_)
    level.implementation->clear();
}

std::vector<std::string> CacheHierarchy::level_names() const {
  std::vector<std::string> result;
  result.reserve(levels_.size());
  for (const auto &level : levels_)
    result.push_back(level.name);
  return result;
}

const CacheConfig &CacheHierarchy::configuration(std::size_t level) const {
  if (level == 0 || level > levels_.size())
    throw std::runtime_error("invalid hierarchy level index");
  return levels_[level - 1].config;
}

HierarchyAccessResult CacheHierarchy::read(std::uint64_t address,
                                           std::size_t size) {
  return access(address, size, AccessType::Read);
}

HierarchyAccessResult CacheHierarchy::write(std::uint64_t address,
                                            std::size_t size) {
  return access(address, size, AccessType::Write);
}

InsertResult CacheHierarchy::insert(std::size_t index, CacheLine line,
                                    HierarchyAccessResult &result) {
  const auto placed = levels_[index].implementation->insert(line);
  check_slot(levels_[index].config, placed.set, placed.way);
  if (placed.evicted) {
    const auto &victim = *placed.evicted;
    if (victim.line_address == line.line_address ||
        victim.line_address % levels_[index].config.line_size_bytes != 0)
      throw std::runtime_error("cache level returned invalid victim address");
    result.evictions.push_back({index + 1, victim});
  }
  return placed;
}

HierarchyAccessResult CacheHierarchy::access(std::uint64_t address,
                                             std::size_t size,
                                             AccessType kind) {
  if (size == 0 ||
      size - 1 > std::numeric_limits<std::uint64_t>::max() - address)
    throw std::runtime_error("invalid access size/address overflow");
  HierarchyAccessResult result;
  const auto line_size = levels_.front().config.line_size_bytes;
  while (size != 0) {
    const auto line_address = address - address % line_size;
    const auto first_result = result.lines.size();
    CacheLine incoming{line_address, false};
    bool found = false;
    for (std::size_t index = 0; index < levels_.size(); ++index) {
      const auto lookup = levels_[index].implementation->lookup(line_address);
      const auto &config = levels_[index].config;
      const auto set_count =
          config.size_bytes / line_size / config.associativity;
      if (lookup.set >= set_count)
        throw std::runtime_error("cache level returned invalid lookup result");
      if (lookup.way)
        check_slot(config, lookup.set, *lookup.way);
      result.lines.push_back({line_address, lookup.hit(), index + 1, lookup.set,
                              lookup.way, std::nullopt});
      if (!lookup.hit())
        continue;
      levels_[index].implementation->touch(
          line_address, index == 0 ? kind : AccessType::Read);
      found = true;
      if (index != 0) {
        const auto extracted =
            levels_[index].implementation->invalidate(line_address);
        if (!extracted || extracted->line_address != line_address)
          throw std::runtime_error("cache level failed to invalidate hit line");
        incoming = *extracted;
      }
      break;
    }
    if (!result.lines[first_result].hit) {
      if (!found)
        ++result.memory_reads;
      incoming.dirty = incoming.dirty || kind == AccessType::Write;
      const auto placed = insert(0, incoming, result);
      auto &first = result.lines[first_result];
      if (first.set != placed.set)
        throw std::runtime_error(
            "cache level changed set between probe and insert");
      first.way = placed.way;
      first.evicted = placed.evicted;
      auto victim = placed.evicted;
      for (std::size_t index = 1; victim && index < levels_.size(); ++index)
        victim = insert(index, *victim, result).evicted;
      if (victim && victim->dirty)
        ++result.memory_writebacks;
    }
    const auto offset = static_cast<std::size_t>(address % line_size);
    const auto consumed = std::min(size, line_size - offset);
    size -= consumed;
    if (size != 0)
      address += consumed;
  }
  return result;
}

} // namespace cache_simulator
