#pragma once

#include "cache_simulator/cache_hierarchy.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cache_simulator {

struct LineStatistics {
  std::uint64_t probes = 0;
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t evictions = 0;
  std::uint64_t writebacks = 0;

  std::optional<double> hit_rate_percent() const;
};

struct LevelStatistics {
  LineStatistics total;
  LineStatistics reads;
  LineStatistics writes;
};

struct InstructionStatistics {
  std::uint64_t memref_id = 0;
  std::string kind;
  std::uint64_t events = 0;
  std::uint64_t requested_bytes = 0;
  std::vector<LevelStatistics> levels;
  std::vector<std::uint64_t> level_events;
};

struct BufferLayout {
  std::uint64_t base_address = 0;
  std::int64_t min_offset = 0;
  std::uint64_t end_offset =
      1; // Exclusive; the reserved range includes offset 0.
};

struct RunSummary {
  std::uint64_t events = 0;
  std::uint64_t reads = 0;
  std::uint64_t writes = 0;
  std::uint64_t read_bytes = 0;
  std::uint64_t write_bytes = 0;
  std::vector<std::string> level_names;
  std::vector<LevelStatistics> levels;
  std::vector<std::uint64_t> level_events;
  std::uint64_t memory_reads = 0;
  std::uint64_t memory_writebacks = 0;
  std::map<std::uint64_t, InstructionStatistics> instructions;
  std::map<std::uint64_t, BufferLayout> buffers;
};

struct CacheProfile {
  std::string id;
  std::string source;
  std::vector<std::string> limitations;
};

// Input must remain unchanged throughout both passes. Throws std::runtime_error
// on invalid input, model failure or I/O failure. A failed replay may leave the
// model partially populated, but never replaces a previously published report.
RunSummary run_trace(const std::filesystem::path &input_path,
                     CacheHierarchy &hierarchy,
                     const std::filesystem::path &report_path,
                     const std::optional<CacheProfile> &profile = std::nullopt);

} // namespace cache_simulator
