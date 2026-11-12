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

const char *to_string(InclusionPolicy p) {
  switch (p) {
  case InclusionPolicy::Exclusive:
    return "exclusive";
  case InclusionPolicy::Inclusive:
    return "inclusive";
  case InclusionPolicy::NonInclusive:
    return "non_inclusive";
  }
  throw std::invalid_argument("invalid inclusion policy");
}
const char *to_string(WritePolicy p) {
  switch (p) {
  case WritePolicy::WriteBack:
    return "write_back";
  case WritePolicy::WriteThrough:
    return "write_through";
  }
  throw std::invalid_argument("invalid write policy");
}
const char *to_string(AllocationPolicy p) {
  switch (p) {
  case AllocationPolicy::WriteAllocate:
    return "write_allocate";
  case AllocationPolicy::NoWriteAllocate:
    return "no_write_allocate";
  }
  throw std::invalid_argument("invalid allocation policy");
}

CacheHierarchy::CacheHierarchy(std::vector<CacheLevel> levels,
                               HierarchyConfig policies)
    : policies_(std::move(policies)), levels_(std::move(levels)) {
  if (levels_.empty())
    throw std::runtime_error("hierarchy must contain at least one level");
  if (policies_.inclusion_policies.empty())
    policies_.inclusion_policies.assign(levels_.size() - 1,
                                        InclusionPolicy::Exclusive);
  if (policies_.inclusion_policies.size() != levels_.size() - 1)
    throw std::runtime_error("provide one inclusion policy per adjacent pair");
  for (auto policy : policies_.inclusion_policies) {
    (void)to_string(policy);
    exclusive_write_back_ &= policy == InclusionPolicy::Exclusive;
  }
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
      throw std::runtime_error("hierarchy requires equal line sizes");
    exclusive_write_back_ &=
        config.write_policy == WritePolicy::WriteBack &&
        config.allocation_policy == AllocationPolicy::WriteAllocate;
    (void)to_string(config.write_policy);
    (void)to_string(config.allocation_policy);
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

// Exclusive WB/WA needs only one lookup per demand probe: no service probes
// or duplicate copies. Keep this path usable by synthetic miss-rate models too.
void CacheHierarchy::exclusive_access(std::uint64_t address, AccessType kind,
                                      HierarchyAccessResult &result) {
  const auto first = result.lines.size();
  CacheLine incoming{address, kind == AccessType::Write};
  bool found = false;
  for (std::size_t index = 0; index < levels_.size(); ++index) {
    const auto lookup = probe(index, address, result);
    if (!lookup.hit())
      continue;
    levels_[index].implementation->touch(address,
                                         index == 0 ? kind : AccessType::Read);
    if (index == 0)
      return;
    const auto extracted = levels_[index].implementation->invalidate(address);
    if (!extracted || extracted->line_address != address)
      throw std::runtime_error("cache level failed to invalidate hit line");
    incoming.dirty = incoming.dirty || extracted->dirty;
    found = true;
    break;
  }
  if (!found)
    ++result.memory_reads;
  for (std::size_t index = 0; index < levels_.size(); ++index) {
    const auto placed = levels_[index].implementation->insert(incoming);
    check_slot(levels_[index].config, placed.set, placed.way);
    if (placed.evicted &&
        (placed.evicted->line_address == incoming.line_address ||
         placed.evicted->line_address % levels_[index].config.line_size_bytes))
      throw std::runtime_error("cache level returned invalid victim address");
    if (index == 0) {
      auto &entry = result.lines[first];
      if (entry.set != placed.set)
        throw std::runtime_error(
            "cache level changed set between probe and insert");
      entry.way = placed.way;
      entry.evicted = placed.evicted;
    }
    if (!placed.evicted)
      return;
    incoming = *placed.evicted;
    result.evictions.push_back({index + 1, incoming});
    if (incoming.dirty)
      result.writebacks.push_back({index + 1, incoming});
  }
  if (incoming.dirty) {
    ++result.memory_writebacks;
    ++result.memory_writes;
    result.memory_write_bytes += levels_.front().config.line_size_bytes;
  }
}

LookupResult CacheHierarchy::lookup(std::size_t index,
                                    std::uint64_t address) const {
  const auto found = levels_[index].implementation->lookup(address);
  const auto &config = levels_[index].config;
  if (found.set >=
      config.size_bytes / config.line_size_bytes / config.associativity)
    throw std::runtime_error("cache level returned invalid lookup result");
  if (found.way)
    check_slot(config, found.set, *found.way);
  return found;
}

LookupResult CacheHierarchy::probe(std::size_t index, std::uint64_t address,
                                   HierarchyAccessResult &result) {
  const auto lookup = this->lookup(index, address);
  // A write-allocate read-for-ownership followed by write-through is one
  // demand probe per level/line, even if servicing it revisits the level.
  const auto existing = std::find_if(
      result.lines.begin(), result.lines.end(), [&](const auto &r) {
        return r.level == index + 1 && r.line_address == address;
      });
  if (existing == result.lines.end())
    result.lines.push_back({address, lookup.hit(), index + 1, lookup.set,
                            lookup.way, std::nullopt});
  return lookup;
}

bool CacheHierarchy::exclusive_copy_above(std::size_t index,
                                          std::uint64_t address) const {
  while (index > 0 && policies_.inclusion_policies[index - 1] ==
                          InclusionPolicy::Exclusive) {
    --index;
    if (lookup(index, address).hit())
      return true;
  }
  return false;
}

CacheLine CacheHierarchy::extract_peers(std::size_t index, CacheLine line) {
  auto first = index;
  auto last = index;
  while (first > 0 &&
         policies_.inclusion_policies[first - 1] == InclusionPolicy::Exclusive)
    --first;
  while (last + 1 < levels_.size() &&
         policies_.inclusion_policies[last] == InclusionPolicy::Exclusive)
    ++last;
  for (auto peer = first; peer <= last; ++peer) {
    if (peer == index)
      continue;
    if (!lookup(peer, line.line_address).hit())
      continue;
    auto extracted =
        levels_[peer].implementation->invalidate(line.line_address);
    if (!extracted || extracted->line_address != line.line_address)
      throw std::runtime_error("cache level failed to invalidate hit line");
    line.dirty = line.dirty || extracted->dirty;
  }
  return line;
}

void CacheHierarchy::fill(std::size_t index, std::uint64_t address, bool dirty,
                          HierarchyAccessResult &result) {
  auto source = index;
  for (; source < levels_.size(); ++source) {
    if (probe(source, address, result).hit()) {
      levels_[source].implementation->touch(address, AccessType::Read);
      break;
    }
  }
  if (source == levels_.size())
    ++result.memory_reads;
  auto last = index;
  while (last + 1 < levels_.size() && last < source) {
    const auto policy = policies_.inclusion_policies[last];
    if (policy == InclusionPolicy::Exclusive ||
        (policy == InclusionPolicy::NonInclusive &&
         !policies_.non_inclusive_fill))
      break;
    ++last;
  }
  // Locate the source before changing residency. This preserves victim-cache
  // behavior: an exclusive miss fetches into the requesting level only.
  for (auto target = last;; --target) {
    install(target, {address, dirty && target == index}, result);
    if (target == index)
      break;
  }
}

void CacheHierarchy::install(std::size_t index, CacheLine line,
                             HierarchyAccessResult &result) {
  line = extract_peers(index, line);
  if (index + 1 < levels_.size() &&
      policies_.inclusion_policies[index] == InclusionPolicy::Inclusive &&
      !lookup(index + 1, line.line_address).hit())
    install(index + 1, {line.line_address, false}, result);

  auto &cache = *levels_[index].implementation;
  const bool through =
      levels_[index].config.write_policy == WritePolicy::WriteThrough;
  auto existing = lookup(index, line.line_address);
  if (existing.hit()) {
    cache.touch(line.line_address,
                line.dirty && !through ? AccessType::Write : AccessType::Read);
  } else {
    const auto placed =
        cache.insert({line.line_address, line.dirty && !through});
    check_slot(levels_[index].config, placed.set, placed.way);
    for (auto &probe : result.lines) {
      if (probe.level == index + 1 && probe.line_address == line.line_address) {
        if (probe.set != placed.set)
          throw std::runtime_error("cache level changed set during fill");
        probe.way = placed.way;
        probe.evicted = placed.evicted;
      }
    }
    if (placed.evicted) {
      if (placed.evicted->line_address == line.line_address ||
          placed.evicted->line_address % levels_[index].config.line_size_bytes)
        throw std::runtime_error("cache level returned invalid victim address");
      evict(index, *placed.evicted, result);
    }
  }
  if (line.dirty && through)
    writeback(index, line, result);
}

void CacheHierarchy::evict(std::size_t index, CacheLine line,
                           HierarchyAccessResult &result) {
  // An inclusive victim must collect the newest dirty data before moving down.
  auto upper = index;
  while (upper > 0 && policies_.inclusion_policies[upper - 1] ==
                          InclusionPolicy::Inclusive) {
    --upper;
    const auto removed =
        levels_[upper].implementation->invalidate(line.line_address);
    if (removed) {
      if (removed->line_address != line.line_address)
        throw std::runtime_error(
            "cache level returned invalid invalidated line");
      result.back_invalidations.push_back({upper + 1, *removed});
      if (removed->dirty)
        result.writebacks.push_back({upper + 1, *removed});
      line.dirty = line.dirty || removed->dirty;
    }
  }
  result.evictions.push_back({index + 1, line});
  if (line.dirty) {
    writeback(index, line, result);
  } else if (index + 1 < levels_.size() &&
             (policies_.inclusion_policies[index] ==
                  InclusionPolicy::Exclusive ||
              policies_.writeback_clean)) {
    install(index + 1, line, result);
  }
}

void CacheHierarchy::writeback(std::size_t from, CacheLine line,
                               HierarchyAccessResult &result) {
  result.writebacks.push_back({from + 1, line});
  auto target = from + 1;
  // A write-through transfer cannot create a second copy in an exclusive
  // segment. Bypass those levels until the next segment or main memory.
  while (target < levels_.size() &&
         exclusive_copy_above(target, line.line_address))
    ++target;
  if (target == levels_.size()) {
    ++result.memory_writebacks;
    ++result.memory_writes;
    result.memory_write_bytes += levels_.front().config.line_size_bytes;
  } else {
    // Full-line service writebacks allocate even with CPU no-write-allocate.
    install(target, line, result);
  }
}

void CacheHierarchy::store(std::size_t index, std::uint64_t address,
                           std::size_t bytes, HierarchyAccessResult &result) {
  if (index == levels_.size()) {
    ++result.memory_writes;
    result.memory_write_bytes += bytes;
    return;
  }
  const auto lookup = probe(index, address, result);
  const auto &config = levels_[index].config;
  const bool through = config.write_policy == WritePolicy::WriteThrough;
  if (exclusive_copy_above(index, address)) {
    store(index + 1, address, bytes, result);
    return;
  }
  if (lookup.hit()) {
    levels_[index].implementation->touch(address, through ? AccessType::Read
                                                          : AccessType::Write);
  } else if (config.allocation_policy == AllocationPolicy::WriteAllocate) {
    fill(index, address, !through, result);
  } else {
    store(index + 1, address, bytes, result);
    return;
  }
  if (through)
    store(index + 1, address, bytes, result);
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
    const auto consumed = std::min(
        size, line_size - static_cast<std::size_t>(address % line_size));
    if (exclusive_write_back_) {
      exclusive_access(line_address, kind, result);
    } else if (kind == AccessType::Write) {
      store(0, line_address, consumed, result);
    } else if (probe(0, line_address, result).hit()) {
      levels_[0].implementation->touch(line_address, AccessType::Read);
    } else {
      fill(0, line_address, false, result);
    }
    size -= consumed;
    if (size != 0)
      address += consumed;
  }
  return result;
}

} // namespace cache_simulator
