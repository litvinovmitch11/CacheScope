#pragma once

#include "cache_simulator/ReplacementPolicy.h"
#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

namespace cache_simulator {

// Uniformly chooses among registered ways. clear() also restores the seed.
class Random final : public IReplacementPolicy {
public:
  explicit Random(std::uint64_t seed = 0);

  void on_hit(std::size_t way) override;
  void on_insert(std::size_t way) override;
  void on_invalidate(std::size_t way) override;
  std::size_t choose_victim() override;
  void clear() override;

private:
  std::uint64_t seed_;
  std::mt19937_64 generator_;
  std::vector<std::size_t> ways_;
  std::unordered_map<std::size_t, std::size_t> positions_;
};

} // namespace cache_simulator
