#pragma once

#include "cache_simulator/ReplacementPolicy.h"
#include <array>

namespace cache_simulator {

// Intel486 four-way tree PLRU: Intel 273021-001, section 7.5, figure 7-2.
struct PLRU4 final : public IReplacementPolicy {
  void on_hit(std::size_t way) override;
  void on_insert(std::size_t way) override;
  void on_invalidate(std::size_t way) override;
  std::size_t choose_victim() override;
  void clear() override;

private:
  void update(std::size_t way);
  std::array<bool, 3> bits_{};
  unsigned valid_ = 0;
};

} // namespace cache_simulator
