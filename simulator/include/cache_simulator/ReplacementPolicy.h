#pragma once

namespace cache_simulator {

struct IReplacementPolicy {
public:
  virtual ~IReplacementPolicy() = default;

  virtual void on_hit(size_t way) = 0;
  virtual void on_insert(size_t way) = 0;
  virtual size_t choose_victim() = 0;
  virtual void clear() = 0;
};

} // namespace cache_simulator