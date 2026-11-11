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
#include <vector>

using namespace cache_simulator;

namespace {
struct Operation {
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
  std::size_t set;
  bool hit;
  std::optional<Line> victim;
};

struct Expected {
  std::vector<Probe> probes;
  std::vector<Line> evictions;
  std::uint64_t reads = 0;
  std::uint64_t writebacks = 0;
};

class Reference {
public:
  Reference(std::size_t sets, std::size_t ways, std::size_t line_size, bool lru)
      : sets_(sets), ways_(ways), line_size_(line_size), lru_(lru) {}

  void clear() {
    for (auto &set : sets_)
      set.clear();
  }

  Expected access(const Operation &op) {
    Expected result;
    const auto first = op.address / line_size_;
    const auto last = (op.address + op.size - 1) / line_size_;
    for (auto block = first; block <= last; ++block) {
      const auto address = block * line_size_;
      const auto index = block % sets_.size();
      auto &set = sets_[index];
      auto found =
          std::find_if(set.begin(), set.end(), [address](const Line &line) {
            return line.address == address;
          });
      Probe probe{address, index, found != set.end(), std::nullopt};
      if (found != set.end()) {
        if (op.write)
          found->dirty = true;
        if (lru_) {
          const Line touched = *found;
          set.erase(found);
          set.push_back(touched);
        }
      } else {
        ++result.reads;
        if (set.size() == ways_) {
          probe.victim = set.front();
          result.evictions.push_back(set.front());
          if (set.front().dirty)
            ++result.writebacks;
          set.erase(set.begin());
        }
        set.push_back({address, op.write});
      }
      result.probes.push_back(probe);
    }
    return result;
  }

private:
  std::vector<std::vector<Line>> sets_;
  std::size_t ways_;
  std::size_t line_size_;
  bool lru_;
};

bool same_line(const std::optional<Line> &expected,
               const std::optional<CacheLine> &actual) {
  if (expected.has_value() != actual.has_value())
    return false;
  return !expected || (expected->address == actual->line_address &&
                       expected->dirty == actual->dirty);
}

std::string mismatch(const Expected &expected,
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
    if (e.address != a.line_address || e.set != a.set || e.hit != a.hit ||
        a.level != 1 || !a.way || !same_line(e.victim, a.evicted))
      return "probe " + std::to_string(i);
  }
  for (std::size_t i = 0; i < expected.evictions.size(); ++i) {
    const auto &e = expected.evictions[i];
    const auto &a = actual.evictions[i];
    if (a.level != 1 || e.address != a.line.line_address ||
        e.dirty != a.line.dirty)
      return "eviction " + std::to_string(i);
  }
  return {};
}

class Comparison {
public:
  Comparison(std::size_t sets, std::size_t ways, bool lru)
      : sets_(sets), ways_(ways), lru_(lru),
        reference_(sets, ways, line_size, lru),
        hierarchy_(make_hierarchy(sets, ways, lru)) {}

  void run(const std::vector<Operation> &trace, const std::string &label) {
    reference_.clear();
    hierarchy_.clear();
    for (std::size_t i = 0; i < trace.size(); ++i) {
      const auto &op = trace[i];
      const auto expected = reference_.access(op);
      const auto actual = op.write ? hierarchy_.write(op.address, op.size)
                                   : hierarchy_.read(op.address, op.size);
      const auto difference = mismatch(expected, actual);
      if (!difference.empty()) {
        std::ostringstream message;
        message << label << ": " << (lru_ ? "LRU" : "FIFO") << " sets=" << sets_
                << " ways=" << ways_ << " line_size=" << line_size
                << " operation=" << i << " mismatch=" << difference
                << "\ntrace:";
        for (std::size_t j = 0; j <= i; ++j)
          message << "\n  " << j << ": " << (trace[j].write ? 'W' : 'R')
                  << " address=" << trace[j].address
                  << " size=" << trace[j].size;
        throw std::runtime_error(message.str());
      }
    }
  }

private:
  static constexpr std::size_t line_size = 4;
  static CacheHierarchy make_hierarchy(std::size_t sets, std::size_t ways,
                                       bool lru) {
    const auto size = sets * ways * line_size;
    ReplacementPolicyFactory factory;
    if (lru)
      factory = [] { return std::make_unique<LRU>(); };
    else
      factory = [] { return std::make_unique<FIFO>(); };
    std::vector<CacheLevel> levels;
    levels.push_back({"L1",
                      {size, line_size, ways, lru ? "LRU" : "FIFO"},
                      std::make_unique<SetAssociativeCacheLevel>(
                          CacheLevelConfig{size, line_size, ways}, factory)});
    return CacheHierarchy(std::move(levels));
  }

  std::size_t sets_;
  std::size_t ways_;
  bool lru_;
  Reference reference_;
  CacheHierarchy hierarchy_;
};

void exhaustive(Comparison &comparison) {
  constexpr std::size_t choices = 8;
  constexpr std::size_t count = choices * choices * choices * choices * choices;
  for (std::size_t sequence = 0; sequence < count; ++sequence) {
    auto digits = sequence;
    std::vector<Operation> trace;
    for (int i = 0; i < 5; ++i) {
      const auto choice = digits % choices;
      digits /= choices;
      trace.push_back({(choice / 2) * 4, 1, choice % 2 != 0});
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
  std::vector<Operation> trace;
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
    for (const bool lru : {true, false}) {
      for (const std::size_t sets : {1, 2}) {
        for (const std::size_t ways : {1, 2}) {
          Comparison comparison(sets, ways, lru);
          exhaustive(comparison);
          generated(comparison);
        }
      }
    }
    std::cout << "reference comparisons passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
