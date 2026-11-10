#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cache_simulator {

struct LineAccessResult {
  std::uint64_t line_address; // starting address of the accessed cache line
  bool hit;                   // true - cache hit, false - cache miss
  std::size_t set;            // index of the cache set containing the line
  std::size_t way; // index of the way containing the line after the access
  std::optional<std::uint64_t>
      evicted_address; // starting address of the evicted line, if any
  std::size_t level;   // 1-based index into ICacheSimulator::level_names()
};

struct AccessResult {
  // Demand probes at every visited level, excluding internal fills/writebacks.
  std::vector<LineAccessResult> lines;
};

struct ICacheSimulator {
public:
  virtual ~ICacheSimulator() = default;

  virtual void clear() = 0;

  // Ordered from L1 outward; includes levels which receive no probes.
  virtual std::vector<std::string> level_names() const = 0;

  virtual AccessResult read(uint64_t address, std::size_t size) = 0;

  virtual AccessResult write(uint64_t address, std::size_t size) = 0;
};

} // namespace cache_simulator
