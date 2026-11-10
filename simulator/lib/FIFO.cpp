#include "cache_simulator/FIFO.h"
#include <stdexcept>

namespace cache_simulator {

void FIFO::on_hit(std::size_t way) {
  const auto position = positions_.find(way);
  if (position == positions_.end()) {
    throw std::out_of_range("cannot touch an unknown FIFO way");
  }
  // FIFO order changes only when a line is inserted.
}

void FIFO::on_insert(std::size_t way) {
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

void FIFO::on_invalidate(std::size_t way) {
  const auto position = positions_.find(way);
  if (position == positions_.end()) {
    return;
  }
  order_.erase(position->second);
  positions_.erase(position);
}

std::size_t FIFO::choose_victim() {
  if (order_.empty()) {
    throw std::out_of_range("cannot choose a victim from an empty FIFO policy");
  }
  return order_.back();
}

void FIFO::clear() {
  positions_.clear();
  order_.clear();
}

} // namespace cache_simulator
