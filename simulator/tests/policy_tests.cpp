#include "cache_simulator/LRU.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include "cache_simulator/cache_hierarchy.hpp"

#include <iostream>
#include <stdexcept>
using namespace cache_simulator;

static void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Setup {
  std::vector<ICacheLevel *> levels;
  CacheHierarchy cache;
  Setup(std::vector<CacheConfig> configs, HierarchyConfig policies = {})
      : cache(make(configs), policies) {}
  std::vector<CacheLevel> make(const std::vector<CacheConfig> &configs) {
    std::vector<CacheLevel> result;
    for (auto config : configs) {
      auto cache = std::make_unique<SetAssociativeCacheLevel>(
          CacheLevelConfig{config.size_bytes, config.line_size_bytes,
                           config.associativity},
          [] { return std::make_unique<LRU>(); });
      levels.push_back(cache.get());
      result.push_back(
          {"L" + std::to_string(result.size() + 1), config, std::move(cache)});
    }
    return result;
  }
};
static void write_through_no_allocate() {
  Setup s({{64, 64, 1, "LRU", WritePolicy::WriteThrough,
            AllocationPolicy::NoWriteAllocate}});
  auto r = s.cache.write(60, 8);
  check(r.memory_reads == 0 && r.memory_writes == 2 &&
            r.memory_write_bytes == 8 && r.memory_writebacks == 0,
        "no-write-allocate bypasses cold lines, writes only requested bytes");
  check(!s.levels[0]->lookup(0).hit() && !s.levels[0]->lookup(64).hit() &&
            r.lines.size() == 2 && !r.lines[0].way,
        "no-write-allocate leaves the cache empty");
  s.cache.read(0, 4);
  r = s.cache.write(4, 4);
  check(r.lines[0].hit && r.memory_writes == 1 &&
            !s.levels[0]->invalidate(0)->dirty,
        "write-through hit writes memory and keeps a clean copy");
}
static void inclusive_dirty_invalidation() {
  Setup s({{128, 64, 2, "LRU"}, {64, 64, 1, "LRU"}},
          {{InclusionPolicy::Inclusive}});
  s.cache.write(0, 4);
  check(s.levels[0]->lookup(0).hit() && s.levels[1]->lookup(0).hit(),
        "inclusive cold fill creates both copies");
  auto r = s.cache.read(64, 4);
  check(!s.levels[0]->lookup(0).hit() && r.back_invalidations.size() == 1 &&
            r.back_invalidations[0].line.dirty && r.memory_writebacks == 1 &&
            r.memory_write_bytes == 64,
        "lower eviction invalidates dirty upper copy and writes latest data "
        "once");
  check(s.cache.read(0, 4).memory_reads == 1,
        "back-invalidated line must be fetched again");
}
static void non_inclusive() {
  for (bool fill : {false, true})
    for (bool clean : {false, true}) {
      Setup s({{64, 64, 1, "LRU"}, {128, 64, 2, "LRU"}},
              {{InclusionPolicy::NonInclusive}, clean, fill});
      s.cache.read(0, 4);
      check(s.levels[1]->lookup(0).hit() == fill,
            "NINE fill policy controls cold lower-level allocation");
      s.cache.read(64, 4);
      check(s.levels[1]->lookup(0).hit() == (fill || clean),
            "NINE clean victims are forwarded only when enabled");
    }
  Setup s({{128, 64, 2, "LRU"}, {64, 64, 1, "LRU"}},
          {{InclusionPolicy::NonInclusive}});
  s.cache.write(0, 4);
  auto r = s.cache.read(64, 4);
  check(s.levels[0]->lookup(0).hit() && r.back_invalidations.empty() &&
            r.memory_writebacks == 0,
        "NINE lower eviction leaves dirty upper copy resident");
  r = s.cache.read(128, 4);
  check(r.memory_writebacks == 0 && s.levels[1]->lookup(0).hit(),
        "NINE dirty upper victim survives in lower cache");
  r = s.cache.read(192, 4);
  check(r.memory_writebacks == 1, "NINE dirty lower victim reaches memory");
}
static void exclusive_write_through() {
  Setup s({{64, 64, 1, "LRU", WritePolicy::WriteThrough}, {64, 64, 1, "LRU"}});
  auto r = s.cache.write(0, 4);
  check(r.memory_reads == 1 && r.memory_writes == 1 &&
            r.memory_write_bytes == 4 && !s.levels[1]->lookup(0).hit(),
        "WT forwards through exclusive peers without duplicating a line");
  s.cache.read(64, 4);
  s.levels[1]->touch(0, AccessType::Write);
  r = s.cache.read(0, 4);
  check(
      r.memory_reads == 0 && r.memory_writebacks == 1 &&
          !s.levels[0]->invalidate(0)->dirty,
      "promotion of a dirty line to WT drains it and keeps the new copy clean");

  Setup nwa({{64, 64, 1, "LRU", WritePolicy::WriteBack,
              AllocationPolicy::NoWriteAllocate},
             {64, 64, 1, "LRU"}});
  r = nwa.cache.write(0, 4);
  check(r.memory_reads == 1 && !nwa.levels[0]->lookup(0).hit() &&
            nwa.levels[1]->lookup(0).hit(),
        "NWA bypasses L1 but permits write-allocate at L2");
  check(nwa.cache.write(0, 4).lines[1].hit,
        "NWA forwarded store can hit the lower cache");
}
static void combinations() {
  const InclusionPolicy choices[] = {InclusionPolicy::Exclusive,
                                     InclusionPolicy::Inclusive,
                                     InclusionPolicy::NonInclusive};
  for (auto a : choices)
    for (auto b : choices)
      for (unsigned mask = 0; mask < 64; ++mask)
        for (unsigned flags = 0; flags < 4; ++flags) {
          std::vector<CacheConfig> configs;
          for (unsigned i = 0; i < 3; ++i)
            configs.push_back(
                {(i + 1) * 64, 64, i + 1, "LRU",
                 (mask & (1U << (2 * i))) ? WritePolicy::WriteThrough
                                          : WritePolicy::WriteBack,
                 (mask & (2U << (2 * i))) ? AllocationPolicy::NoWriteAllocate
                                          : AllocationPolicy::WriteAllocate});
          Setup s(configs, {{a, b}, bool(flags & 1), bool(flags & 2)});
          std::uint32_t random = 42;
          for (unsigned n = 0; n < 150; ++n) {
            random = random * 1664525 + 1013904223;
            auto address = (random >> 8) % 512;
            auto r = (random & 1) ? s.cache.read(address, 9)
                                  : s.cache.write(address, 9);
            check(r.memory_writes >= r.memory_writebacks,
                  "memory writebacks are a subset of writes");
            for (unsigned line = 0; line < 576; line += 64)
              for (unsigned i = 0; i < 2; ++i) {
                const auto policy = i == 0 ? a : b;
                if (policy == InclusionPolicy::Inclusive &&
                    s.levels[i]->lookup(line).hit())
                  check(s.levels[i + 1]->lookup(line).hit(),
                        "inclusive subset invariant");
                if (policy == InclusionPolicy::Exclusive &&
                    s.levels[i]->lookup(line).hit())
                  check(!s.levels[i + 1]->lookup(line).hit(),
                        "exclusive adjacency invariant");
                if (a == InclusionPolicy::Exclusive && b == a &&
                    s.levels[0]->lookup(line).hit())
                  check(!s.levels[2]->lookup(line).hit(),
                        "exclusive segment uniqueness");
              }
          }
          for (unsigned i = 0; i < 3; ++i)
            for (unsigned line = 0; line < 576; line += 64) {
              auto resident = s.levels[i]->invalidate(line);
              if (resident &&
                  configs[i].write_policy == WritePolicy::WriteThrough)
                check(!resident->dirty,
                      "WT lines stay clean for every policy combination");
            }
        }
}
int main() {
  try {
    write_through_no_allocate();
    inclusive_dirty_invalidation();
    non_inclusive();
    exclusive_write_through();
    combinations();
    std::cout << "cache policies passed (2304 combinations)\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
