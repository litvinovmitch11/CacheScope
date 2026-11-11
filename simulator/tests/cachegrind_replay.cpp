#include "cache_simulator/LRU.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include "cache_simulator/cache_hierarchy.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

using namespace cache_simulator;

int main() {
  std::vector<CacheLevel> levels;
  levels.push_back({"L1D",
                    {32768, 64, 8, "LRU"},
                    std::make_unique<SetAssociativeCacheLevel>(
                        CacheLevelConfig{32768, 64, 8},
                        [] { return std::make_unique<LRU>(); })});
  CacheHierarchy cache(std::move(levels));
  char kind;
  std::uint64_t address;
  std::size_t reads = 0, read_misses = 0, writes = 0, write_misses = 0;
  while (std::cin >> kind >> address) {
    if (kind != 'R' && kind != 'W') {
      std::cerr << "invalid access kind\n";
      return 1;
    }
    const auto result =
        kind == 'R' ? cache.read(address, 1) : cache.write(address, 1);
    if (result.lines.size() != 1) {
      std::cerr << "expected a single line probe\n";
      return 1;
    }
    if (kind == 'R') {
      ++reads;
      read_misses += !result.lines[0].hit;
    } else {
      ++writes;
      write_misses += !result.lines[0].hit;
    }
  }
  if (!std::cin.eof())
    return 1;
  std::cout << reads << ' ' << read_misses << ' ' << writes << ' '
            << write_misses << '\n';
}
