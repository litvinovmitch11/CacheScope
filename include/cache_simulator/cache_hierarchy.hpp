#pragma once

#include "cache_simulator/CacheLevel.h"

#include <memory>
#include <string>
#include <vector>

namespace cache_simulator {

struct CacheConfig {
  std::size_t size_bytes;
  std::size_t line_size_bytes;
  std::size_t associativity;
  std::string replacement_policy;
};

struct CacheLevel {
  std::string name;
  CacheConfig config;
  std::unique_ptr<ICacheLevel> implementation;
};

struct HierarchyLineAccessResult {
  std::uint64_t line_address;
  bool hit;
  std::size_t level; // 1-based, L1 first.
  std::size_t set;
  std::optional<std::size_t> way; // A lower-level miss does not allocate.
  std::optional<CacheLine> evicted;
};

struct HierarchyEviction {
  std::size_t level;
  CacheLine line;
};

struct HierarchyAccessResult {
  std::vector<HierarchyLineAccessResult> lines; // Demand probes only.
  std::vector<HierarchyEviction> evictions;     // Includes victim migrations.
  std::uint64_t memory_reads = 0;
  std::uint64_t memory_writebacks = 0;
};

// Exclusive, write-back/write-allocate hierarchy with equal line sizes.
// Each resident line belongs to exactly one level. Levels own dirty state.
class CacheHierarchy {
public:
  explicit CacheHierarchy(std::vector<CacheLevel> levels);

  void clear();
  std::vector<std::string> level_names() const;
  const CacheConfig &configuration(std::size_t level) const;

  HierarchyAccessResult read(std::uint64_t address, std::size_t size);
  HierarchyAccessResult write(std::uint64_t address, std::size_t size);

private:
  HierarchyAccessResult access(std::uint64_t address, std::size_t size,
                               AccessType kind);
  InsertResult insert(std::size_t level, CacheLine line,
                      HierarchyAccessResult &result);

  std::vector<CacheLevel> levels_;
};

} // namespace cache_simulator
