#include "cache_simulator/LRU.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include "cache_simulator/cache_hierarchy.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

using namespace cache_simulator;

int main() {
  std::vector<CacheLevel> levels;
  levels.push_back({"L1D",
                    {128, 64, 2, "LRU"},
                    std::make_unique<SetAssociativeCacheLevel>(
                        CacheLevelConfig{128, 64, 2},
                        [] { return std::make_unique<LRU>(); })});
  CacheHierarchy cache(std::move(levels));
  char kind;
  std::uint64_t address;
  std::size_t hits = 0;
  std::size_t misses = 0;
  while (std::cin >> kind >> address) {
    if (kind != 'R' && kind != 'W') {
      std::cerr << "invalid access kind\n";
      return 1;
    }
    const auto result =
        kind == 'R' ? cache.read(address, 1) : cache.write(address, 1);
    if (result.lines.size() != 1) {
      std::cerr << "expected exactly one line probe\n";
      return 1;
    }
    if (result.lines[0].hit)
      ++hits;
    else
      ++misses;
    std::cout << hits << ' ' << misses << '\n';
  }
  return std::cin.eof() ? 0 : 1;
}
