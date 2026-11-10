#include "cache_simulator/LRU.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include "cache_simulator/cache_hierarchy.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace cache_simulator;
using U64 = std::uint64_t;

static void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

namespace {
struct Setup {
  std::vector<SetAssociativeCacheLevel *> levels;
  CacheHierarchy hierarchy;

  explicit Setup(const std::vector<std::size_t> &sizes)
      : hierarchy(make_levels(sizes, levels)) {}

private:
  static std::vector<CacheLevel>
  make_levels(const std::vector<std::size_t> &sizes,
              std::vector<SetAssociativeCacheLevel *> &view) {
    std::vector<CacheLevel> result;
    for (std::size_t index = 0; index < sizes.size(); ++index) {
      auto model = std::make_unique<SetAssociativeCacheLevel>(
          CacheLevelConfig{sizes[index] * 64, 64, sizes[index]},
          [] { return std::make_unique<LRU>(); });
      view.push_back(model.get());
      result.push_back({"L" + std::to_string(index + 1),
                        {sizes[index] * 64, 64, sizes[index], "LRU"},
                        std::move(model)});
    }
    return result;
  }
};
} // namespace

static void exclusive(const Setup &setup) {
  for (U64 address = 0; address < 256; address += 64) {
    unsigned copies = 0;
    for (const auto *level : setup.levels)
      copies += level->lookup(address).hit() ? 1U : 0U;
    check(copies <= 1, "exclusive hierarchy must not duplicate resident lines");
  }
}

template <class Action> static void fails(Action action) {
  try {
    action();
  } catch (const std::runtime_error &) {
    return;
  }
  throw std::runtime_error("expected hierarchy validation failure");
}

static void migration() {
  Setup setup({1, 1});
  auto &hierarchy = setup.hierarchy;
  auto result = hierarchy.read(0, 4);
  check(result.memory_reads == 1 && result.lines.size() == 2,
        "cold read probes all levels and fetches once");
  check(setup.levels[0]->lookup(0).hit() && !setup.levels[1]->lookup(0).hit(),
        "cold fill allocates only in L1");
  result = hierarchy.write(0, 4);
  check(result.lines.size() == 1 && result.lines[0].hit,
        "write hit stays in L1");
  result = hierarchy.read(64, 4);
  check(setup.levels[1]->lookup(0).hit() && result.evictions[0].line.dirty,
        "write hit sets dirty and victim moves down intact");
  exclusive(setup);
  result = hierarchy.read(0, 4);
  check(result.lines.size() == 2 && result.lines[1].hit &&
            result.memory_reads == 0,
        "L2 hit avoids memory");
  check(setup.levels[0]->lookup(0).hit() && !setup.levels[1]->lookup(0).hit() &&
            setup.levels[1]->lookup(64).hit(),
        "promotion extracts line and exchanges victims");
  exclusive(setup);
  result = hierarchy.read(128, 4);
  check(result.memory_writebacks == 0 && result.evictions[0].line.dirty &&
            setup.levels[1]->lookup(0).hit(),
        "promotion preserves dirty; clean last-level victim does not write "
        "memory");
  result = hierarchy.read(192, 4);
  check(result.memory_writebacks == 1 && result.evictions.size() == 2 &&
            result.evictions.back().line.dirty &&
            result.evictions.back().line.line_address == 0,
        "dirty last-level victim writes memory, including address zero");
  exclusive(setup);
  hierarchy.clear();
  for (const auto *level : setup.levels)
    for (U64 address = 0; address < 256; address += 64)
      check(!level->lookup(address).hit(), "clear every level");

  Setup three({1, 1, 1});
  three.hierarchy.write(0, 4);
  three.hierarchy.read(64, 4);
  result = three.hierarchy.read(128, 4);
  check(three.levels[2]->lookup(0).hit() && result.evictions.back().line.dirty,
        "dirty survives two downward migrations");
  result = three.hierarchy.read(0, 4);
  check(result.lines.size() == 3 && !result.lines[1].way &&
            result.lines[2].hit && result.memory_reads == 0,
        "three-level promotion with non-allocating L2 miss");
  check(three.levels[0]->lookup(0).hit() &&
            three.levels[1]->lookup(128).hit() &&
            three.levels[2]->lookup(64).hit(),
        "cascaded promotion preserves all resident lines");
  exclusive(three);
  const auto promoted = three.levels[0]->invalidate(0);
  check(promoted && promoted->dirty, "three-level promotion preserves dirty");
}

static void validation() {
  fails([] { CacheHierarchy hierarchy({}); });
  fails([] {
    std::vector<CacheLevel> levels;
    levels.push_back({"L1", {64, 64, 1, "LRU"}, nullptr});
    CacheHierarchy hierarchy(std::move(levels));
  });
  fails([] {
    std::vector<CacheLevel> levels;
    for (std::size_t line_size : {64, 128})
      levels.push_back({"L",
                        {line_size, line_size, 1, "LRU"},
                        std::make_unique<SetAssociativeCacheLevel>(
                            CacheLevelConfig{line_size, line_size, 1},
                            [] { return std::make_unique<LRU>(); })});
    CacheHierarchy hierarchy(std::move(levels));
  });
  Setup setup({2});
  fails([&] { setup.hierarchy.read(0, 0); });
  fails([&] { setup.hierarchy.read(std::numeric_limits<U64>::max(), 2); });
  fails([&] { setup.hierarchy.configuration(0); });
  auto result = setup.hierarchy.read(60, 8);
  check(result.lines.size() == 2 && result.memory_reads == 2 &&
            result.lines[0].line_address == 0 &&
            result.lines[1].line_address == 64,
        "cross-line accesses split once");
  result = setup.hierarchy.read(std::numeric_limits<U64>::max(), 1);
  check(result.lines.size() == 1, "last byte of uint64 address space is valid");
  const auto names = setup.hierarchy.level_names();
  check(names.size() == 1 && names[0] == "L1", "level metadata");
}

static void lru() {
  Setup setup({2});
  auto &hierarchy = setup.hierarchy;
  hierarchy.read(0, 4);
  hierarchy.read(64, 4);
  check(hierarchy.write(0, 4).lines[0].hit, "touch resident line");
  auto result = hierarchy.read(128, 4);
  check(result.evictions.size() == 1 &&
            result.evictions[0].line.line_address == 64 &&
            result.memory_writebacks == 0,
        "demand hit updates real LRU replacement state");
  result = hierarchy.read(192, 4);
  check(result.evictions[0].line.line_address == 0 &&
            result.evictions[0].line.dirty && result.memory_writebacks == 1,
        "dirty LRU victim writes back from a single-level hierarchy");
}

int main() {
  try {
    migration();
    validation();
    lru();
    std::cout << "hierarchy passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
