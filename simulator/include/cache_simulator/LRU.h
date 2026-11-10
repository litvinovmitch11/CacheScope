#pragma once

#include "cache_simulator/ReplacementPolicy.h"
#include <list>
#include <unordered_map>

namespace cache_simulator {

struct LRU final : public IReplacementPolicy {
private:
  std::list<std::size_t> order_;
  std::unordered_map<std::size_t, std::list<std::size_t>::iterator> positions_;

public:
  LRU() = default;
  LRU(const LRU &) = delete;
  LRU &operator=(const LRU &) = delete;

  void on_hit(std::size_t way) override;
  void on_insert(std::size_t way) override;
  std::size_t choose_victim() override;
  void clear() override;
};

} // namespace cache_simulator
