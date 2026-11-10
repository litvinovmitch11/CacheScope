#pragma once

#include "cache_simulator/ReplacementPolicy.h"
#include <list>
#include <unordered_map>

namespace cache_simulator {

struct FIFO final : public IReplacementPolicy {
private:
  std::list<std::size_t> order_;
  std::unordered_map<std::size_t, std::list<std::size_t>::iterator> positions_;

public:
  FIFO() = default;
  FIFO(const FIFO &) = delete;
  FIFO &operator=(const FIFO &) = delete;

  void on_hit(std::size_t way) override;
  void on_insert(std::size_t way) override;
  void on_invalidate(std::size_t way) override;
  std::size_t choose_victim() override;
  void clear() override;
};

} // namespace cache_simulator
