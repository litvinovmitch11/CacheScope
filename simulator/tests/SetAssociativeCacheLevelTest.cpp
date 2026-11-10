#include "cache_simulator/SetAssociativeCacheLevel.h"
#include <algorithm>
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace cache_simulator;

namespace {

struct Policy final : IReplacementPolicy {
  std::vector<std::size_t> order;
  bool invalid_victim = false;
  void on_hit(std::size_t way) override { on_insert(way); }
  void on_insert(std::size_t way) override {
    order.erase(std::remove(order.begin(), order.end(), way), order.end());
    order.push_back(way);
  }
  std::size_t choose_victim() override {
    if (order.empty()) {
      throw std::logic_error("empty test policy");
    }
    return invalid_victim ? std::numeric_limits<std::size_t>::max()
                          : order.front();
  }
  void clear() override { order.clear(); }
};

ReplacementPolicyFactory factory() {
  return [] { return std::make_unique<Policy>(); };
}

TEST(SetAssociativeCacheLevelTest, Operations) {
  SetAssociativeCacheLevel cache({256, 64, 2}, factory());
  const auto lookup = cache.lookup(64);
  EXPECT_FALSE(lookup.hit());
  EXPECT_EQ(lookup.set, 1u);
  auto first = cache.insert({0, false});
  EXPECT_EQ(first.set, 0u);
  EXPECT_EQ(first.way, 0u);
  EXPECT_FALSE(first.evicted.has_value());
  cache.insert({128, true});
  EXPECT_TRUE(cache.lookup(0).hit());
  auto replacement = cache.insert({256, false});
  ASSERT_TRUE(replacement.evicted.has_value());
  EXPECT_EQ(replacement.evicted->line_address, 0u);
  cache.touch(128, AccessType::Read);
  replacement = cache.insert({384, false});
  ASSERT_TRUE(replacement.evicted.has_value());
  EXPECT_EQ(replacement.evicted->line_address, 256u);
  cache.touch(384, AccessType::Write);
  auto removed = cache.invalidate(384);
  ASSERT_TRUE(removed.has_value());
  EXPECT_TRUE(removed->dirty);
  EXPECT_FALSE(cache.invalidate(384).has_value());
  auto inserted = cache.insert({512, false});
  EXPECT_EQ(inserted.way, replacement.way);
  EXPECT_FALSE(inserted.evicted.has_value());
  replacement = cache.insert({640, false});
  ASSERT_TRUE(replacement.evicted.has_value());
  EXPECT_EQ(replacement.evicted->line_address, 128u);
  EXPECT_TRUE(replacement.evicted->dirty);
  cache.insert({64, false}); // A different set must be independent.
  cache.insert({192, false});
  cache.touch(640, AccessType::Read);
  replacement = cache.insert({320, false});
  EXPECT_EQ(replacement.set, 1u);
  ASSERT_TRUE(replacement.evicted.has_value());
  EXPECT_EQ(replacement.evicted->line_address, 64u);
  EXPECT_TRUE(cache.lookup(640).hit());
  cache.clear();
  EXPECT_FALSE(cache.lookup(640).hit());
  EXPECT_FALSE(cache.lookup(192).hit());
  EXPECT_EQ(cache.insert({0, false}).way, 0u);
  cache.insert({128, false});
  replacement = cache.insert({256, false});
  ASSERT_TRUE(replacement.evicted.has_value());
  EXPECT_EQ(replacement.evicted->line_address, 0u);
}

TEST(SetAssociativeCacheLevelTest, Errors) {
  SetAssociativeCacheLevel cache({128, 64, 2}, factory());
  cache.insert({0, false});
  cache.insert({64, true});
  EXPECT_THROW(([&] { cache.insert({0, true}); })(), std::invalid_argument);
  EXPECT_THROW(([&] { cache.lookup(1); })(), std::invalid_argument);
  EXPECT_THROW(([&] { cache.touch(1, AccessType::Write); })(),
               std::invalid_argument);
  EXPECT_THROW(([&] { cache.insert({1, false}); })(), std::invalid_argument);
  EXPECT_THROW(([&] { cache.invalidate(1); })(), std::invalid_argument);
  EXPECT_THROW(([&] { cache.touch(0, static_cast<AccessType>(99)); })(),
               std::invalid_argument);
  EXPECT_THROW(([&] { cache.touch(128, AccessType::Write); })(),
               std::out_of_range);
  const auto replacement = cache.insert({128, false});
  ASSERT_TRUE(replacement.evicted.has_value());
  EXPECT_EQ(replacement.evicted->line_address, 0u);
  const auto clean = cache.invalidate(128);
  ASSERT_TRUE(clean.has_value());
  EXPECT_FALSE(clean->dirty);
  const auto dirty = cache.invalidate(64);
  ASSERT_TRUE(dirty.has_value());
  EXPECT_TRUE(dirty->dirty);

  std::vector<CacheLevelConfig> invalid = {
      {0, 64, 1},   {64, 0, 1},
      {64, 64, 0},  {128, 3, 1},
      {65, 64, 1},  {64, 64, 2},
      {192, 64, 1}, {128, 64, std::numeric_limits<std::size_t>::max()}};
  for (const auto &config : invalid) {
    EXPECT_THROW(([&] { SetAssociativeCacheLevel level(config, factory()); })(),
                 std::invalid_argument);
  }
  EXPECT_THROW(([] { SetAssociativeCacheLevel level({64, 64, 1}, {}); })(),
               std::invalid_argument);
  EXPECT_THROW(([] {
                 SetAssociativeCacheLevel level({64, 64, 1}, [] {
                   return std::unique_ptr<IReplacementPolicy>{};
                 });
               })(),
               std::invalid_argument);
}

TEST(SetAssociativeCacheLevelTest, Geometries) {
  SetAssociativeCacheLevel direct({128, 64, 1}, factory());
  direct.insert({0, false});
  direct.insert({64, true});
  const auto direct_replacement = direct.insert({128, false});
  ASSERT_TRUE(direct_replacement.evicted.has_value());
  EXPECT_EQ(direct_replacement.evicted->line_address, 0u);
  EXPECT_TRUE(direct.lookup(64).hit());

  SetAssociativeCacheLevel fully({192, 64, 3}, factory());
  for (std::uint64_t address : {0, 64, 128}) {
    EXPECT_EQ(fully.insert({address, false}).set, 0u);
  }
  const auto fully_replacement = fully.insert({192, false});
  ASSERT_TRUE(fully_replacement.evicted.has_value());
  EXPECT_EQ(fully_replacement.evicted->line_address, 0u);

  SetAssociativeCacheLevel bytes({2, 1, 1}, factory());
  const auto highest = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(bytes.insert({highest, true}).set, 1u);
  const auto removed = bytes.invalidate(highest);
  ASSERT_TRUE(removed.has_value());
  EXPECT_TRUE(removed->dirty);
}

TEST(SetAssociativeCacheLevelTest, PolicyContract) {
  std::vector<Policy *> policies;
  SetAssociativeCacheLevel cache({256, 64, 2}, [&] {
    auto policy = std::make_unique<Policy>();
    policies.push_back(policy.get());
    return policy;
  });
  ASSERT_EQ(policies.size(), 2u);
  EXPECT_NE(policies[0], policies[1]);
  cache.insert({0, false});
  cache.insert({128, false});
  cache.invalidate(0);
  EXPECT_FALSE(cache.insert({256, false}).evicted.has_value());
  EXPECT_EQ(policies[0]->order, (std::vector<std::size_t>{1, 0}));
  EXPECT_TRUE(policies[1]->order.empty());
  policies[0]->invalid_victim = true;
  EXPECT_THROW(([&] { cache.insert({384, false}); })(), std::logic_error);
  EXPECT_TRUE(cache.lookup(128).hit());
  EXPECT_TRUE(cache.lookup(256).hit());
}
} // namespace
