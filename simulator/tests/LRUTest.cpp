#include "cache_simulator/LRU.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include <gtest/gtest.h>
#include <stdexcept>
#include <type_traits>

namespace cache_simulator {
namespace {

static_assert(!std::is_copy_constructible_v<LRU>);
static_assert(!std::is_copy_assignable_v<LRU>);

TEST(LRUTest, ChoosesOldestWithoutRemovingIt) {
  LRU policy;
  policy.on_insert(7);
  policy.on_insert(3);
  policy.on_insert(42);
  EXPECT_EQ(policy.choose_victim(), 7u);
  EXPECT_EQ(policy.choose_victim(), 7u);
  policy.on_hit(7);
  EXPECT_EQ(policy.choose_victim(), 3u);
  policy.on_hit(3);
  EXPECT_EQ(policy.choose_victim(), 42u);
}

TEST(LRUTest, ReinsertRefreshesExistingWay) {
  LRU policy;
  policy.on_insert(0);
  policy.on_insert(1);
  policy.on_insert(0);
  EXPECT_EQ(policy.choose_victim(), 1u);
  policy.on_hit(1);
  EXPECT_EQ(policy.choose_victim(), 0u);
  policy.on_insert(0);
  EXPECT_EQ(policy.choose_victim(), 1u);
}

TEST(LRUTest, SingleWayCanBeReused) {
  LRU policy;
  for (int iteration = 0; iteration < 10; ++iteration) {
    policy.on_insert(5);
    policy.on_hit(5);
    EXPECT_EQ(policy.choose_victim(), 5u);
  }
}

TEST(LRUTest, RejectsUnknownHitsWithoutChangingOrder) {
  LRU policy;
  EXPECT_THROW(policy.on_hit(0), std::out_of_range);
  EXPECT_THROW(policy.choose_victim(), std::out_of_range);
  policy.on_insert(1);
  policy.on_insert(2);
  EXPECT_THROW(policy.on_hit(0), std::out_of_range);
  EXPECT_EQ(policy.choose_victim(), 1u);
}

TEST(LRUTest, ClearResetsAllState) {
  LRU policy;
  policy.on_insert(0);
  policy.on_insert(1);
  policy.clear();
  policy.clear();
  EXPECT_THROW(policy.choose_victim(), std::out_of_range);
  EXPECT_THROW(policy.on_hit(0), std::out_of_range);
  policy.on_insert(1);
  policy.on_insert(0);
  EXPECT_EQ(policy.choose_victim(), 1u);
}

TEST(LRUTest, IntegratesWithCacheLevelAndIndependentSets) {
  SetAssociativeCacheLevel cache({256, 64, 2},
                                 [] { return std::make_unique<LRU>(); });
  cache.insert({0, true});
  cache.insert({128, false});
  cache.insert({64, false});
  cache.insert({192, false});
  EXPECT_TRUE(cache.lookup(0).hit()); // Lookup must not update LRU.
  auto replacement = cache.insert({256, false});
  ASSERT_TRUE(replacement.evicted.has_value());
  EXPECT_EQ(replacement.evicted->line_address, 0u);
  EXPECT_TRUE(replacement.evicted->dirty);
  cache.touch(64, AccessType::Read);
  replacement = cache.insert({320, false});
  ASSERT_TRUE(replacement.evicted.has_value());
  EXPECT_EQ(replacement.evicted->line_address, 192u);
  cache.invalidate(128);
  replacement = cache.insert({384, false});
  EXPECT_FALSE(replacement.evicted.has_value());
  replacement = cache.insert({512, false});
  ASSERT_TRUE(replacement.evicted.has_value());
  EXPECT_EQ(replacement.evicted->line_address, 256u);
  cache.clear();
  EXPECT_FALSE(cache.lookup(512).hit());
  cache.insert({0, false});
  cache.insert({128, false});
  replacement = cache.insert({256, false});
  ASSERT_TRUE(replacement.evicted.has_value());
  EXPECT_EQ(replacement.evicted->line_address, 0u);
}

} // namespace
} // namespace cache_simulator
