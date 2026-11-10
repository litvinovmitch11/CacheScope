#include "cache_simulator/FIFO.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include <gtest/gtest.h>
#include <stdexcept>

namespace cache_simulator {
namespace {
TEST(FIFOTest, HitsDoNotChangeInsertionOrder) {
  FIFO policy;
  policy.on_insert(7);
  policy.on_insert(3);
  policy.on_insert(42);
  policy.on_hit(7);
  EXPECT_EQ(policy.choose_victim(), 7u);
  EXPECT_EQ(policy.choose_victim(), 7u);
  policy.on_insert(7);
  EXPECT_EQ(policy.choose_victim(), 3u);
  policy.on_insert(3);
  EXPECT_EQ(policy.choose_victim(), 42u);
}

TEST(FIFOTest, InvalidateRemovesMiddleOldestAndNewest) {
  FIFO policy;
  policy.on_insert(0);
  policy.on_insert(1);
  policy.on_insert(2);
  policy.on_invalidate(1);
  EXPECT_THROW(policy.on_hit(1), std::out_of_range);
  EXPECT_EQ(policy.choose_victim(), 0u);
  policy.on_invalidate(0);
  EXPECT_EQ(policy.choose_victim(), 2u);
  policy.on_insert(1);
  policy.on_invalidate(1);
  EXPECT_EQ(policy.choose_victim(), 2u);
  policy.on_invalidate(2);
  policy.on_invalidate(2);
  policy.on_invalidate(99);
  EXPECT_THROW(policy.choose_victim(), std::out_of_range);
  policy.on_insert(1);
  EXPECT_EQ(policy.choose_victim(), 1u);
}

TEST(FIFOTest, ClearAndErrorsPreserveConsistentState) {
  FIFO policy;
  EXPECT_THROW(policy.choose_victim(), std::out_of_range);
  EXPECT_THROW(policy.on_hit(0), std::out_of_range);
  policy.on_insert(5);
  policy.on_insert(5);
  EXPECT_THROW(policy.on_hit(1), std::out_of_range);
  EXPECT_EQ(policy.choose_victim(), 5u);
  policy.on_invalidate(5);
  EXPECT_THROW(policy.choose_victim(), std::out_of_range);
  policy.on_insert(0);
  policy.clear();
  policy.clear();
  EXPECT_THROW(policy.on_hit(0), std::out_of_range);
  EXPECT_THROW(policy.choose_victim(), std::out_of_range);
  policy.on_insert(1);
  policy.on_insert(0);
  EXPECT_EQ(policy.choose_victim(), 1u);
}

TEST(FIFOTest, CacheLevelEvictsOldestDespiteHitsAndReusesInvalidatedWay) {
  SetAssociativeCacheLevel cache({128, 64, 2},
                                 [] { return std::make_unique<FIFO>(); });
  cache.insert({0, true});
  cache.insert({64, false});
  cache.touch(0, AccessType::Read);
  const auto replacement = cache.insert({128, false});
  ASSERT_TRUE(replacement.evicted.has_value());
  EXPECT_EQ(replacement.evicted->line_address, 0u);
  EXPECT_TRUE(replacement.evicted->dirty);
  cache.invalidate(64);
  EXPECT_FALSE(cache.insert({192, false}).evicted.has_value());
  const auto next = cache.insert({256, false});
  ASSERT_TRUE(next.evicted.has_value());
  EXPECT_EQ(next.evicted->line_address, 128u);
}
} // namespace
} // namespace cache_simulator
