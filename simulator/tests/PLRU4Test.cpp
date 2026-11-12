#include "cache_simulator/PLRU4.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include <gtest/gtest.h>
#include <stdexcept>

namespace cache_simulator {
namespace {

TEST(PLRU4Test, MatchesEveryBranchOfIntelFigure7_2) {
  // Each pair reaches one of the eight B0/B1/B2 states in Intel's tree.
  const unsigned cases[][3] = {{0, 2, 1}, {0, 3, 1}, {1, 2, 0}, {1, 3, 0},
                               {2, 0, 3}, {2, 1, 3}, {3, 0, 2}, {3, 1, 2}};
  for (const auto &row : cases) {
    PLRU4 policy;
    for (unsigned way = 0; way < 4; ++way)
      policy.on_insert(way);
    policy.on_hit(row[0]);
    policy.on_hit(row[1]);
    EXPECT_EQ(policy.choose_victim(), row[2]);
    EXPECT_EQ(policy.choose_victim(), row[2]);
  }
}

TEST(PLRU4Test, EvictsTreeVictimInsteadOfGlobalLRUAndPreservesDirty) {
  SetAssociativeCacheLevel cache({64, 16, 4},
                                 [] { return std::make_unique<PLRU4>(); });
  cache.insert({0, false});
  cache.insert({16, false});
  cache.insert({32, true});
  cache.insert({48, false});
  cache.touch(0, AccessType::Read);
  // Exact LRU would evict 16; the Intel tree selects 32.
  auto placed = cache.insert({64, false});
  ASSERT_TRUE(placed.evicted);
  EXPECT_EQ(placed.evicted->line_address, 32u);
  EXPECT_TRUE(placed.evicted->dirty);
  EXPECT_TRUE(cache.lookup(16).hit());
}

TEST(PLRU4Test, PrefersFirstFreeWayAndKeepsSetsIndependent) {
  SetAssociativeCacheLevel cache({128, 16, 4},
                                 [] { return std::make_unique<PLRU4>(); });
  for (unsigned address = 0; address < 128; address += 16)
    cache.insert({address, false});
  cache.touch(0, AccessType::Read);
  auto placed = cache.insert({128, false});
  ASSERT_TRUE(placed.evicted);
  EXPECT_EQ(placed.evicted->line_address, 64u);
  placed = cache.insert({144, false});
  ASSERT_TRUE(placed.evicted);
  EXPECT_EQ(placed.evicted->line_address, 16u);
  cache.invalidate(0);
  placed = cache.insert({160, false});
  EXPECT_FALSE(placed.evicted);
  EXPECT_EQ(placed.way, 0u);
  cache.clear();
  for (unsigned address = 0; address < 128; address += 16)
    cache.insert({address, false});
  EXPECT_EQ(cache.insert({128, false}).evicted->line_address, 0u);
}

TEST(PLRU4Test, RejectsInvalidHitsAndResetsState) {
  PLRU4 policy;
  EXPECT_THROW(policy.choose_victim(), std::out_of_range);
  EXPECT_THROW(policy.on_hit(0), std::out_of_range);
  EXPECT_THROW(policy.on_insert(4), std::out_of_range);
  for (unsigned way = 0; way < 4; ++way)
    policy.on_insert(way);
  EXPECT_THROW(policy.on_hit(4), std::out_of_range);
  EXPECT_EQ(policy.choose_victim(), 0u);
  policy.on_invalidate(0);
  EXPECT_THROW(policy.on_hit(0), std::out_of_range);
  policy.on_invalidate(99);
  policy.on_insert(0);
  EXPECT_EQ(policy.choose_victim(), 2u);
  policy.clear();
  EXPECT_THROW(policy.on_hit(0), std::out_of_range);
  for (unsigned way = 0; way < 4; ++way)
    policy.on_insert(way);
  EXPECT_EQ(policy.choose_victim(), 0u);
}

} // namespace
} // namespace cache_simulator
