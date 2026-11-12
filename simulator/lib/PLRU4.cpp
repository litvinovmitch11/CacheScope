#include "cache_simulator/PLRU4.h"
#include <stdexcept>

namespace cache_simulator {

void PLRU4::update(std::size_t way) {
  bits_[0] = way < 2;
  bits_[way < 2 ? 1 : 2] = (way % 2) == 0;
}

void PLRU4::on_hit(std::size_t way) {
  if (way >= 4 || !(valid_ & (1U << way)))
    throw std::out_of_range("cannot touch an unknown PLRU4 way");
  update(way);
}

void PLRU4::on_insert(std::size_t way) {
  if (way >= 4)
    throw std::out_of_range("PLRU4 requires exactly four ways");
  valid_ |= 1U << way;
  update(way);
}

void PLRU4::on_invalidate(std::size_t way) {
  // Intel updates the tree on hits/fills; invalidation only removes validity.
  if (way < 4)
    valid_ &= ~(1U << way);
}

std::size_t PLRU4::choose_victim() {
  // SetAssociativeCacheLevel selects the first free way before consulting us.
  if (valid_ != 15)
    throw std::out_of_range("PLRU4 victim selection requires a full set");
  const std::size_t group = bits_[0] ? 2 : 0;
  return group + (bits_[group == 0 ? 1 : 2] ? 1 : 0);
}

void PLRU4::clear() {
  valid_ = 0;
  bits_.fill(false);
}

} // namespace cache_simulator
