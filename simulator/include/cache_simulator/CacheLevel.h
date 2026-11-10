#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace cache_simulator {

enum class AccessType { Read, Write };

struct CacheLine {
  std::uint64_t line_address;
  bool dirty = false;
};

struct LookupResult {
  std::size_t set;
  std::optional<std::size_t> way; // Present on hit; nullopt on miss.

  bool hit() const noexcept { return way.has_value(); }
};

struct InsertResult {
  std::size_t set;
  std::size_t way;
  std::optional<CacheLine> evicted;
};

// One cache level. The caller splits byte ranges into aligned cache lines.
struct ICacheLevel {
  virtual ~ICacheLevel() = default;

  // Remove all lines and reset replacement state.
  virtual void clear() = 0;

  // Check presence without changing dirty or replacement state.
  // On miss, way is nullopt and no line is allocated.
  virtual LookupResult lookup(std::uint64_t line_address) const = 0;

  // Update replacement state; Write also marks the existing line dirty.
  // Throw std::out_of_range if the line is absent, without changing state.
  virtual void touch(std::uint64_t line_address, AccessType type) = 0;

  // Remove and return the line, preserving dirty; nullopt if absent.
  // Removal also updates replacement state so the way can be reused.
  virtual std::optional<CacheLine> invalidate(std::uint64_t line_address) = 0;

  // Insert an absent line, preserving dirty and updating replacement state.
  // Prefer a free way; otherwise return the evicted line, including dirty.
  virtual InsertResult insert(CacheLine line) = 0;
};

} // namespace cache_simulator
