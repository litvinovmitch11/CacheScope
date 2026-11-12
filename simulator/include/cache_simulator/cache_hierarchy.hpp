#pragma once

#include "cache_simulator/CacheLevel.h"

#include <memory>
#include <string>
#include <vector>

namespace cache_simulator {

enum class InclusionPolicy { Exclusive, Inclusive, NonInclusive };
enum class WritePolicy { WriteBack, WriteThrough };
enum class AllocationPolicy { WriteAllocate, NoWriteAllocate };

const char *to_string(InclusionPolicy policy);
const char *to_string(WritePolicy policy);
const char *to_string(AllocationPolicy policy);

struct HierarchyConfig {
  // One entry per adjacent pair, L1/L2 first. Empty means all exclusive.
  std::vector<InclusionPolicy> inclusion_policies;
  bool writeback_clean = false;
  bool non_inclusive_fill = true;
};

struct CacheConfig {
  std::size_t size_bytes;
  std::size_t line_size_bytes;
  std::size_t associativity;
  std::string replacement_policy;
  WritePolicy write_policy = WritePolicy::WriteBack;
  AllocationPolicy allocation_policy = AllocationPolicy::WriteAllocate;
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
  std::optional<std::size_t>
      way; // Observed probe/fill slot, not a final occupancy snapshot.
  std::optional<CacheLine> evicted;
};

struct HierarchyEviction {
  std::size_t level;
  CacheLine line;
};

struct HierarchyAccessResult {
  std::vector<HierarchyLineAccessResult> lines; // Demand probes only.
  std::vector<HierarchyEviction> evictions;     // Includes victim migrations.
  std::vector<HierarchyEviction> back_invalidations;
  std::vector<HierarchyEviction>
      writebacks; // Dirty transfers to the next level.
  std::uint64_t memory_reads = 0;
  std::uint64_t memory_writes = 0;
  std::uint64_t memory_write_bytes = 0;
  std::uint64_t memory_writebacks = 0;
};

// Configurable hierarchy with equal line sizes. Levels own dirty state.
// Defaults preserve the exclusive, write-back/write-allocate model.
class CacheHierarchy {
public:
  explicit CacheHierarchy(std::vector<CacheLevel> levels,
                          HierarchyConfig policies = {});

  const HierarchyConfig &policies() const { return policies_; }
  void clear();
  std::vector<std::string> level_names() const;
  const CacheConfig &configuration(std::size_t level) const;

  HierarchyAccessResult read(std::uint64_t address, std::size_t size);
  HierarchyAccessResult write(std::uint64_t address, std::size_t size);

private:
  HierarchyAccessResult access(std::uint64_t address, std::size_t size,
                               AccessType kind);
  void exclusive_access(std::uint64_t address, AccessType kind,
                        HierarchyAccessResult &result);
  LookupResult lookup(std::size_t index, std::uint64_t address) const;
  LookupResult probe(std::size_t index, std::uint64_t address,
                     HierarchyAccessResult &result);
  bool exclusive_copy_above(std::size_t index, std::uint64_t address) const;
  CacheLine extract_peers(std::size_t index, CacheLine line);
  void fill(std::size_t index, std::uint64_t address, bool dirty,
            HierarchyAccessResult &result);
  void install(std::size_t index, CacheLine line,
               HierarchyAccessResult &result);
  void evict(std::size_t index, CacheLine line, HierarchyAccessResult &result);
  void writeback(std::size_t from, CacheLine line,
                 HierarchyAccessResult &result);
  void store(std::size_t index, std::uint64_t address, std::size_t bytes,
             HierarchyAccessResult &result);

  bool exclusive_write_back_ = true;
  HierarchyConfig policies_;
  std::vector<CacheLevel> levels_;
};

} // namespace cache_simulator
