#pragma once

#include "cache_simulator/trace_runner.hpp"
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <stdexcept>

namespace cache_simulator {

class TraceChanged : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Preflights the complete input, spooling normalized events to temporary disk.
// Owns the replay cursor/statistics; the caller owns the referenced hierarchy.
// One event is completed per step. Calls must be serialized by the caller.
// An execution failure requires reset before further execution/statistics
// reads. JSON returned here uses artifact numeric types; HTTP DTO conversion is
// separate.
class TraceReplay {
public:
  TraceReplay(const std::filesystem::path &trace, CacheHierarchy &hierarchy,
              const std::function<bool()> &cancelled = {});
  ~TraceReplay();
  TraceReplay(const TraceReplay &) = delete;
  TraceReplay &operator=(const TraceReplay &) = delete;

  std::uint64_t total_events() const;
  std::uint64_t cursor() const;
  const RunSummary &summary() const;
  nlohmann::json metadata() const;
  nlohmann::json instructions(std::uint64_t offset, std::size_t limit) const;
  nlohmann::json statistics() const;
  nlohmann::json step();
  void reset();
  void check_unchanged() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace cache_simulator
