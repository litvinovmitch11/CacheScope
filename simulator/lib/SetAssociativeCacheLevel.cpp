#include "cache_simulator/SetAssociativeCacheLevel.h"
#include <limits>
#include <stdexcept>

namespace cache_simulator {

namespace {

bool is_power_of_two(std::size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

std::size_t validate_config(const CacheLevelConfig &config) {
  if (!is_power_of_two(config.line_size_bytes) || config.associativity == 0 ||
      config.capacity_bytes == 0) {
    throw std::invalid_argument("invalid cache dimensions");
  }
  if (config.associativity >
      std::numeric_limits<std::size_t>::max() / config.line_size_bytes) {
    throw std::invalid_argument("cache set size overflows");
  }
  const auto set_bytes = config.line_size_bytes * config.associativity;
  if (config.capacity_bytes % set_bytes != 0) {
    throw std::invalid_argument("cache capacity must contain full sets");
  }
  const auto set_count = config.capacity_bytes / set_bytes;
  if (!is_power_of_two(set_count)) {
    throw std::invalid_argument("cache set count must be a power of two");
  }
  return set_count;
}

} // namespace

SetAssociativeCacheLevel::SetAssociativeCacheLevel(
    CacheLevelConfig config, const ReplacementPolicyFactory &policy_factory)
    : config_(config) {
  const auto set_count = validate_config(config);
  if (!policy_factory) {
    throw std::invalid_argument("replacement policy factory is empty");
  }
  sets_.reserve(set_count);
  for (std::size_t index = 0; index < set_count; ++index) {
    auto policy = policy_factory();
    if (!policy) {
      throw std::invalid_argument("replacement policy factory returned null");
    }
    policy->clear();
    sets_.push_back(
        {std::vector<std::optional<CacheLine>>(config.associativity),
         std::move(policy)});
  }
}

std::size_t
SetAssociativeCacheLevel::compute_set_index(std::uint64_t line_address) const {
  if (line_address % config_.line_size_bytes != 0) {
    throw std::invalid_argument("cache line address is not aligned");
  }
  return (line_address / config_.line_size_bytes) % sets_.size();
}

LookupResult
SetAssociativeCacheLevel::lookup(std::uint64_t line_address) const {
  const auto index = compute_set_index(line_address);
  const auto &lines = sets_[index].lines;
  for (std::size_t way = 0; way < lines.size(); ++way) {
    if (lines[way] && lines[way]->line_address == line_address) {
      return {index, way};
    }
  }
  return {index, std::nullopt};
}

void SetAssociativeCacheLevel::touch(std::uint64_t line_address,
                                     AccessType type) {
  if (type != AccessType::Read && type != AccessType::Write) {
    throw std::invalid_argument("invalid cache access type");
  }
  const auto result = lookup(line_address);
  if (!result.hit()) {
    throw std::out_of_range("cannot touch an absent cache line");
  }
  auto &set = sets_[result.set];
  set.policy->on_hit(*result.way);
  if (type == AccessType::Write) {
    set.lines[*result.way]->dirty = true;
  }
}

InsertResult SetAssociativeCacheLevel::insert(CacheLine line) {
  const auto result = lookup(line.line_address);
  if (result.hit()) {
    throw std::invalid_argument("cannot insert an existing cache line");
  }
  auto &set = sets_[result.set];
  std::size_t way = 0;
  while (way < set.lines.size() && set.lines[way]) {
    ++way;
  }
  if (way == set.lines.size()) {
    way = set.policy->choose_victim();
    if (way >= set.lines.size()) {
      throw std::logic_error("replacement policy returned an invalid way");
    }
  }
  const auto evicted = set.lines[way];
  set.policy->on_insert(way);
  set.lines[way] = line;
  return {result.set, way, evicted};
}

std::optional<CacheLine>
SetAssociativeCacheLevel::invalidate(std::uint64_t line_address) {
  const auto result = lookup(line_address);
  if (!result.hit()) {
    return std::nullopt;
  }
  auto &line = sets_[result.set].lines[*result.way];
  const auto removed = line;
  sets_[result.set].policy->on_invalidate(*result.way);
  line.reset();
  return removed;
}

void SetAssociativeCacheLevel::clear() {
  for (auto &set : sets_) {
    set.policy->clear();
    for (auto &line : set.lines) {
      line.reset();
    }
  }
}

} // namespace cache_simulator
