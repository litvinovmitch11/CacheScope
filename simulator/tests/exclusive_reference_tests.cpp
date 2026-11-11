#include "cache_simulator/FIFO.h"
#include "cache_simulator/LRU.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include "cache_simulator/cache_hierarchy.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace cache_simulator;

namespace {
constexpr std::size_t line_size = 4;

struct Config {
  std::size_t sets;
  std::size_t ways;
};
struct Access {
  std::uint64_t address;
  std::size_t size;
  bool write;
};
struct Line {
  std::uint64_t address;
  bool dirty;
};
struct Probe {
  std::uint64_t address;
  std::size_t level;
  std::size_t set;
  bool hit;
  std::optional<Line> victim;
};
struct Eviction {
  std::size_t level;
  Line line;
};
struct Result {
  std::vector<Probe> probes;
  std::vector<Eviction> evictions;
  std::size_t reads = 0;
  std::size_t writebacks = 0;
};

// Each set is an oldest-first list of address/dirty pairs. The model does not
// use cache levels, replacement policies, or ways from the implementation.
class Reference {
public:
  Reference(const std::vector<Config> &configs, bool lru) : lru_(lru) {
    for (const auto &config : configs)
      levels_.push_back(
          {config.ways, std::vector<std::vector<Line>>(config.sets)});
  }
  void clear() {
    for (auto &level : levels_)
      for (auto &set : level.sets)
        set.clear();
  }
  bool contains(std::size_t level, std::uint64_t address) const {
    const auto &sets = levels_[level].sets;
    const auto &set = sets[(address / line_size) % sets.size()];
    return std::any_of(set.begin(), set.end(), [address](const Line &line) {
      return line.address == address;
    });
  }
  Result access(const Access &access) {
    Result result;
    const auto last = (access.address + access.size - 1) / line_size;
    for (auto block = access.address / line_size; block <= last; ++block) {
      const auto address = block * line_size;
      const auto first_probe = result.probes.size();
      std::optional<Line> incoming;
      std::size_t found_level = levels_.size();
      for (std::size_t i = 0; i < levels_.size(); ++i) {
        auto &sets = levels_[i].sets;
        const auto index = block % sets.size();
        auto &set = sets[index];
        const auto found =
            std::find_if(set.begin(), set.end(), [address](const Line &line) {
              return line.address == address;
            });
        const bool hit = found != set.end();
        result.probes.push_back({address, i + 1, index, hit, std::nullopt});
        if (!hit)
          continue;
        found_level = i;
        Line line = *found;
        if (i == 0) {
          line.dirty = line.dirty || access.write;
          if (lru_) {
            set.erase(found);
            set.push_back(line);
          } else {
            *found = line;
          }
        } else {
          set.erase(found);
          incoming = line;
        }
        break;
      }
      if (found_level == 0)
        continue;
      if (!incoming) {
        ++result.reads;
        incoming = Line{address, false};
      }
      incoming->dirty = incoming->dirty || access.write;
      for (std::size_t i = 0; i < levels_.size() && incoming; ++i) {
        auto &level = levels_[i];
        auto &set =
            level.sets[(incoming->address / line_size) % level.sets.size()];
        std::optional<Line> evicted;
        if (set.size() == level.ways) {
          evicted = set.front();
          set.erase(set.begin());
          result.evictions.push_back({i + 1, *evicted});
          if (i == 0)
            result.probes[first_probe].victim = evicted;
        }
        set.push_back(*incoming);
        incoming = evicted;
      }
      if (incoming && incoming->dirty)
        ++result.writebacks;
    }
    return result;
  }

private:
  struct Level {
    std::size_t ways;
    std::vector<std::vector<Line>> sets;
  };
  bool lru_;
  std::vector<Level> levels_;
};

bool same_line(const std::optional<Line> &expected,
               const std::optional<CacheLine> &actual) {
  if (expected.has_value() != actual.has_value())
    return false;
  return !expected || (expected->address == actual->line_address &&
                       expected->dirty == actual->dirty);
}

std::string mismatch(const Result &expected,
                     const HierarchyAccessResult &actual) {
  if (expected.probes.size() != actual.lines.size())
    return "probe count";
  if (expected.reads != actual.memory_reads ||
      expected.writebacks != actual.memory_writebacks)
    return "memory reads/writebacks";
  if (expected.evictions.size() != actual.evictions.size())
    return "eviction count";
  for (std::size_t i = 0; i < expected.probes.size(); ++i) {
    const auto &e = expected.probes[i];
    const auto &a = actual.lines[i];
    const bool way_expected = e.level == 1 || e.hit;
    if (e.address != a.line_address || e.level != a.level || e.set != a.set ||
        e.hit != a.hit || way_expected != a.way.has_value() ||
        !same_line(e.victim, a.evicted))
      return "probe " + std::to_string(i);
  }
  for (std::size_t i = 0; i < expected.evictions.size(); ++i) {
    const auto &e = expected.evictions[i];
    const auto &a = actual.evictions[i];
    if (e.level != a.level || e.line.address != a.line.line_address ||
        e.line.dirty != a.line.dirty)
      return "eviction " + std::to_string(i);
  }
  return {};
}

class Comparison {
public:
  Comparison(std::vector<Config> configs, bool lru)
      : configs_(std::move(configs)), lru_(lru), reference_(configs_, lru_),
        hierarchy_(make_hierarchy()) {}
  void run(const std::vector<Access> &trace, const std::string &label) {
    reference_.clear();
    hierarchy_.clear();
    for (std::size_t i = 0; i < trace.size(); ++i) {
      const auto &op = trace[i];
      const auto expected = reference_.access(op);
      const auto actual = op.write ? hierarchy_.write(op.address, op.size)
                                   : hierarchy_.read(op.address, op.size);
      auto difference = mismatch(expected, actual);
      for (std::uint64_t address = 0; difference.empty() && address < 64;
           address += line_size) {
        unsigned copies = 0;
        for (std::size_t level = 0; level < views_.size(); ++level) {
          const bool resident = views_[level]->lookup(address).hit();
          copies += resident ? 1U : 0U;
          if (resident != reference_.contains(level, address))
            difference = "residency level=" + std::to_string(level + 1) +
                         " address=" + std::to_string(address);
        }
        if (copies > 1)
          difference = "exclusive invariant address=" + std::to_string(address);
      }
      if (!difference.empty()) {
        std::ostringstream message;
        message << label << ": " << (lru_ ? "LRU" : "FIFO") << " levels=";
        for (const auto &config : configs_)
          message << "(" << config.sets << " sets," << config.ways << " ways)";
        message << " line_size=" << line_size << " operation=" << i
                << " mismatch=" << difference << "\ntrace:";
        for (std::size_t j = 0; j <= i; ++j)
          message << "\n  " << j << ": " << (trace[j].write ? 'W' : 'R')
                  << " address=" << trace[j].address
                  << " size=" << trace[j].size;
        throw std::runtime_error(message.str());
      }
    }
  }

private:
  CacheHierarchy make_hierarchy() {
    std::vector<CacheLevel> levels;
    for (std::size_t i = 0; i < configs_.size(); ++i) {
      const auto config = configs_[i];
      const auto size = config.sets * config.ways * line_size;
      ReplacementPolicyFactory factory;
      if (lru_)
        factory = [] { return std::make_unique<LRU>(); };
      else
        factory = [] { return std::make_unique<FIFO>(); };
      auto level = std::make_unique<SetAssociativeCacheLevel>(
          CacheLevelConfig{size, line_size, config.ways}, factory);
      views_.push_back(level.get());
      levels.push_back({"L" + std::to_string(i + 1),
                        {size, line_size, config.ways, lru_ ? "LRU" : "FIFO"},
                        std::move(level)});
    }
    return CacheHierarchy(std::move(levels));
  }
  std::vector<Config> configs_;
  bool lru_;
  Reference reference_;
  std::vector<SetAssociativeCacheLevel *> views_;
  CacheHierarchy hierarchy_;
};

void golden_reference() {
  Reference reference({{1, 1}, {1, 1}, {1, 1}}, true);
  const auto run = [&](std::uint64_t address, bool write) {
    return reference.access({address, 1, write});
  };
  run(0, true);
  run(4, false);
  run(8, false);
  auto promotion = run(0, false);
  if (promotion.probes.size() != 3 || !promotion.probes[2].hit ||
      promotion.reads != 0 || promotion.evictions.size() != 2 ||
      promotion.evictions[0].line.address != 8 ||
      promotion.evictions[1].line.address != 4)
    throw std::runtime_error("golden reference: three-level promotion");
  run(12, false);
  run(16, false);
  auto writeback = run(20, false);
  if (writeback.evictions.size() != 3 || writeback.writebacks != 1 ||
      writeback.evictions.back().line.address != 0 ||
      !writeback.evictions.back().line.dirty)
    throw std::runtime_error("golden reference: dirty writeback");
}

void exhaustive(Comparison &comparison) {
  constexpr std::size_t choices = 8;
  constexpr std::size_t count = choices * choices * choices * choices * choices;
  for (std::size_t sequence = 0; sequence < count; ++sequence) {
    auto digits = sequence;
    std::vector<Access> trace;
    for (int i = 0; i < 5; ++i) {
      const auto choice = digits % choices;
      digits /= choices;
      trace.push_back({(choice / 2) * line_size, 1, choice % 2 != 0});
    }
    comparison.run(trace, "exhaustive sequence=" + std::to_string(sequence));
  }
}
void generated(Comparison &comparison) {
  std::uint32_t state = 0x12345678U;
  auto next = [&state] {
    state = state * 1664525U + 1013904223U;
    return state;
  };
  std::vector<Access> trace;
  for (int i = 0; i < 2000; ++i) {
    const auto address = (next() >> 16) % 48;
    const auto size = 1 + (next() >> 16) % 9;
    const bool write = ((next() >> 16) & 1U) != 0;
    trace.push_back({address, size, write});
  }
  comparison.run(trace, "generated seed=0x12345678");
}
} // namespace

int main() {
  try {
    golden_reference();
    const std::vector<std::vector<Config>> configurations = {
        {{1, 1}, {1, 1}},
        {{1, 2}, {2, 1}},
        {{2, 1}, {1, 2}},
        {{1, 1}, {1, 1}, {1, 1}},
        {{1, 2}, {2, 1}, {1, 2}}};
    for (const bool lru : {true, false}) {
      for (const auto &config : configurations) {
        Comparison comparison(config, lru);
        exhaustive(comparison);
        generated(comparison);
      }
    }
    std::cout << "exclusive reference comparisons passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
