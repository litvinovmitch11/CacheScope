#include "cache_simulator/LRU.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include "cache_simulator/cache_hierarchy.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace cache_simulator;

int main(int argc, char **argv) {
  if (argc > 2 || (argc == 2 && std::string(argv[1]) != "3")) {
    std::cerr << "usage: drcachesim_exclusive_replay [3]\n";
    return 1;
  }
  const std::size_t count = argc == 2 ? 3 : 2;
  std::vector<CacheLevel> levels;
  for (const char *name : {"L1D", "L2"})
    levels.push_back({name,
                      {128, 64, 2, "LRU"},
                      std::make_unique<SetAssociativeCacheLevel>(
                          CacheLevelConfig{128, 64, 2},
                          [] { return std::make_unique<LRU>(); })});
  if (count == 3)
    levels.push_back({"L3",
                      {128, 64, 2, "LRU"},
                      std::make_unique<SetAssociativeCacheLevel>(
                          CacheLevelConfig{128, 64, 2},
                          [] { return std::make_unique<LRU>(); })});
  CacheHierarchy cache(std::move(levels));
  std::size_t hits[3] = {}, misses[3] = {};
  char kind;
  std::uint64_t address;
  while (std::cin >> kind >> address) {
    if (kind != 'R' && kind != 'W') {
      std::cerr << "invalid access kind\n";
      return 1;
    }
    const auto result =
        kind == 'R' ? cache.read(address, 1) : cache.write(address, 1);
    for (const auto &line : result.lines) {
      if (line.level < 1 || line.level > count) {
        std::cerr << "invalid cache level\n";
        return 1;
      }
      if (line.hit)
        ++hits[line.level - 1];
      else
        ++misses[line.level - 1];
    }
    std::cout << hits[0] << ' ' << misses[0] << ' ' << hits[1] << ' '
              << misses[1];
    if (count == 3)
      std::cout << ' ' << hits[2] << ' ' << misses[2];
    std::cout << '\n';
  }
  return std::cin.eof() ? 0 : 1;
}
