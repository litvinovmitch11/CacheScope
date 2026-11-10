#include "cache_simulator/LRU.h"
#include <stdexcept>

namespace cache_simulator {

void LRU::on_hit(std::size_t way) {
  const auto position = positions_.find(way);
  if (position == positions_.end()) {
    throw std::out_of_range("cannot touch an unknown LRU way");
  }
  order_.splice(order_.begin(), order_, position->second);
}

void LRU::on_insert(std::size_t way) {
  const auto position = positions_.find(way);
  if (position != positions_.end()) {
    order_.splice(order_.begin(), order_, position->second);
    return;
  }
  order_.push_front(way);
  try {
    positions_.emplace(way, order_.begin());
  } catch (...) {
    order_.pop_front();
    throw;
  }
}

std::size_t LRU::choose_victim() {
  if (order_.empty()) {
    throw std::out_of_range("cannot choose a victim from an empty LRU policy");
  }
  return order_.back();
}

void LRU::clear() {
  positions_.clear();
  order_.clear();
}

} // namespace cache_simulator
