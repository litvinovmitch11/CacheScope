#pragma once

#include <cstddef>

namespace cache_simulator {

struct IReplacementPolicy {
public:
  virtual ~IReplacementPolicy() = default;

  virtual void on_hit(std::size_t way) = 0;
  virtual void on_insert(std::size_t way) = 0;
  virtual std::size_t choose_victim() = 0;
  virtual void clear() = 0;
};

} // namespace cache_simulator
