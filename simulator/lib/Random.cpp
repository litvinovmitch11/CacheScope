#include "cache_simulator/Random.h"
#include <stdexcept>

namespace cache_simulator {

Random::Random(std::uint64_t seed) : seed_(seed), generator_(seed) {}

void Random::on_hit(std::size_t way) {
  if (positions_.find(way) == positions_.end()) {
    throw std::out_of_range("cannot touch an unknown Random way");
  }
}

void Random::on_insert(std::size_t way) {
  if (positions_.find(way) != positions_.end()) {
    return;
  }
  ways_.push_back(way);
  try {
    positions_.emplace(way, ways_.size() - 1);
  } catch (...) {
    ways_.pop_back();
    throw;
  }
}

void Random::on_invalidate(std::size_t way) {
  const auto position = positions_.find(way);
  if (position == positions_.end()) {
    return;
  }
  const auto index = position->second;
  const auto last_way = ways_.back();
  ways_[index] = last_way;
  positions_.at(last_way) = index;
  ways_.pop_back();
  positions_.erase(position);
}

std::size_t Random::choose_victim() {
  if (ways_.empty()) {
    throw std::out_of_range(
        "cannot choose a victim from an empty Random policy");
  }
  std::uniform_int_distribution<std::size_t> distribution(0, ways_.size() - 1);
  return ways_[distribution(generator_)];
}

void Random::clear() {
  ways_.clear();
  positions_.clear();
  generator_.seed(seed_);
}

} // namespace cache_simulator
