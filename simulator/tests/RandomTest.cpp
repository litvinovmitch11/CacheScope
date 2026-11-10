#include "cache_simulator/Random.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include <gtest/gtest.h>
#include <stdexcept>
#include <vector>

namespace cache_simulator {
namespace {
TEST(RandomTest, ChoosesOnlyRegisteredWaysWithoutRemovingThem) {
  Random policy(42);
  policy.on_insert(7);
  policy.on_insert(99);
  for (int iteration = 0; iteration < 100; ++iteration) {
    const auto victim = policy.choose_victim();
    EXPECT_TRUE(victim == 7 || victim == 99);
  }
}

TEST(RandomTest, SeedAndClearReproduceSequence) {
  Random first(42);
  Random second(42);
  for (auto *policy : {&first, &second}) {
    policy->on_insert(5);
    policy->on_insert(11);
    policy->on_insert(42);
  }
  std::vector<std::size_t> sequence;
  for (int iteration = 0; iteration < 100; ++iteration) {
    sequence.push_back(first.choose_victim());
    EXPECT_EQ(sequence.back(), second.choose_victim());
  }
  first.clear();
  first.on_insert(5);
  first.on_insert(11);
  first.on_insert(42);
  for (const auto victim : sequence) {
    EXPECT_EQ(first.choose_victim(), victim);
  }
}

TEST(RandomTest, HitsAndDuplicateInsertsDoNotAffectSelection) {
  Random first(10);
  Random second(10);
  for (auto *policy : {&first, &second}) {
    policy->on_insert(7);
    policy->on_insert(8);
  }
  first.on_hit(7);
  first.on_insert(7);
  first.on_invalidate(99);
  EXPECT_THROW(first.on_hit(99), std::out_of_range);
  for (int iteration = 0; iteration < 100; ++iteration) {
    EXPECT_EQ(first.choose_victim(), second.choose_victim());
  }
}

TEST(RandomTest, InvalidateRemovesWaysAndAllowsReinsertion) {
  Random policy;
  EXPECT_THROW(policy.choose_victim(), std::out_of_range);
  policy.on_insert(1);
  policy.on_insert(2);
  policy.on_insert(3);
  policy.on_invalidate(2);
  policy.on_invalidate(1);
  EXPECT_THROW(policy.on_hit(2), std::out_of_range);
  EXPECT_EQ(policy.choose_victim(), 3u);
  policy.on_invalidate(3);
  policy.on_invalidate(3);
  EXPECT_THROW(policy.choose_victim(), std::out_of_range);
  policy.on_insert(2);
  EXPECT_EQ(policy.choose_victim(), 2u);
  policy.clear();
  policy.clear();
  EXPECT_THROW(policy.on_hit(2), std::out_of_range);
  EXPECT_THROW(policy.choose_victim(), std::out_of_range);
}

TEST(RandomTest, CacheLevelReturnsActualVictimAndReplaysAfterClear) {
  SetAssociativeCacheLevel cache({128, 64, 2},
                                 [] { return std::make_unique<Random>(42); });
  auto run = [&cache] {
    std::vector<std::uint64_t> victims;
    cache.insert({0, true});
    cache.insert({64, true});
    for (std::uint64_t address = 128; address < 1024; address += 64) {
      const auto result = cache.insert({address, true});
      if (!result.evicted) {
        ADD_FAILURE() << "full set did not evict a line";
        break;
      }
      EXPECT_FALSE(cache.lookup(result.evicted->line_address).hit());
      EXPECT_TRUE(cache.lookup(address).hit());
      EXPECT_TRUE(result.evicted->dirty);
      victims.push_back(result.evicted->line_address);
    }
    return victims;
  };
  const auto first = run();
  cache.clear();
  EXPECT_EQ(run(), first);
}
} // namespace
} // namespace cache_simulator
