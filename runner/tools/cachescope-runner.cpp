#include "cache_simulator/FIFO.h"
#include "cache_simulator/LRU.h"
#include "cache_simulator/Random.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include "cache_simulator/trace_runner.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct LevelOptions {
  std::string name;
  std::size_t size_bytes;
  std::size_t line_size_bytes;
  std::size_t associativity;
  std::string replacement_policy;
  std::uint64_t random_seed = 42;
};

struct Options {
  fs::path trace;
  fs::path output_dir;
  std::string cache_name = "L1";
  std::size_t size_bytes = 32 * 1024;
  std::size_t line_size_bytes = 64;
  std::size_t associativity = 8;
  std::string replacement_policy = "lru";
  std::uint64_t random_seed = 42;
  std::vector<LevelOptions> cache_levels;
  bool help = false;
};

void print_usage(std::ostream &output) {
  output << "Usage: cachescope-runner --trace <trace.json> --output-dir <dir> "
            "[options]\n\n"
         << "Options:\n"
         << "  --cache-name <name>       Cache level name (default: L1)\n"
         << "  --size-bytes <n>          Cache capacity (default: 32768)\n"
         << "  --line-size-bytes <n>     Cache line size (default: 64)\n"
         << "  --associativity <n>       Number of ways (default: 8)\n"
         << "  --replacement-policy <p> lru, fifo or random (default: lru)\n"
         << "  --random-seed <n>         Random policy seed (default: 42)\n"
         << "  --cache-level <spec>      Repeatable hierarchy level: "
            "NAME:SIZE:LINE:WAYS:POLICY[:SEED]\n"
         << "  -h, --help                Show this help\n\n"
         << "When --cache-level is present, it replaces the single-level "
            "options. All hierarchy levels must use the same line size.\n"
         << "The runner uses an exclusive write-back/write-allocate hierarchy "
            "and writes "
            "statistics.json and events.json into the output directory.\n";
}

std::string_view value_after(int &index, int argc, char **argv,
                             std::string_view option) {
  if (++index >= argc)
    throw std::invalid_argument("missing value after " + std::string(option));
  return argv[index];
}

std::size_t parse_size(std::string_view text, std::string_view option) {
  std::uint64_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value == 0 || value > std::numeric_limits<std::size_t>::max())
    throw std::invalid_argument(std::string(option) +
                                " must be a positive size_t integer");
  return static_cast<std::size_t>(value);
}

std::uint64_t parse_u64(std::string_view text, std::string_view option) {
  std::uint64_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
    throw std::invalid_argument(std::string(option) +
                                " must be an unsigned 64-bit integer");
  return value;
}

std::vector<std::string_view> split_level_spec(std::string_view text) {
  std::vector<std::string_view> fields;
  while (true) {
    const auto separator = text.find(':');
    fields.push_back(text.substr(0, separator));
    if (separator == std::string_view::npos)
      return fields;
    text.remove_prefix(separator + 1);
  }
}

LevelOptions parse_level(std::string_view text) {
  const auto fields = split_level_spec(text);
  if (fields.size() != 5 && fields.size() != 6)
    throw std::invalid_argument(
        "--cache-level must be NAME:SIZE:LINE:WAYS:POLICY[:SEED]");
  if (fields[0].empty())
    throw std::invalid_argument("--cache-level name must not be empty");

  LevelOptions level{
      std::string(fields[0]), parse_size(fields[1], "--cache-level SIZE"),
      parse_size(fields[2], "--cache-level LINE"),
      parse_size(fields[3], "--cache-level WAYS"), std::string(fields[4])};
  if (level.replacement_policy != "lru" && level.replacement_policy != "fifo" &&
      level.replacement_policy != "random")
    throw std::invalid_argument(
        "--cache-level POLICY must be lru, fifo or random");
  if (fields.size() == 6) {
    if (level.replacement_policy != "random")
      throw std::invalid_argument(
          "--cache-level SEED is only valid for random policy");
    level.random_seed = parse_u64(fields[5], "--cache-level SEED");
  }
  return level;
}

Options parse_options(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      options.help = true;
    } else if (argument == "--trace") {
      options.trace = value_after(index, argc, argv, argument);
    } else if (argument == "--output-dir") {
      options.output_dir = value_after(index, argc, argv, argument);
    } else if (argument == "--cache-name") {
      options.cache_name = value_after(index, argc, argv, argument);
    } else if (argument == "--size-bytes") {
      options.size_bytes =
          parse_size(value_after(index, argc, argv, argument), argument);
    } else if (argument == "--line-size-bytes") {
      options.line_size_bytes =
          parse_size(value_after(index, argc, argv, argument), argument);
    } else if (argument == "--associativity") {
      options.associativity =
          parse_size(value_after(index, argc, argv, argument), argument);
    } else if (argument == "--replacement-policy") {
      options.replacement_policy = value_after(index, argc, argv, argument);
    } else if (argument == "--random-seed") {
      options.random_seed =
          parse_u64(value_after(index, argc, argv, argument), argument);
    } else if (argument == "--cache-level") {
      options.cache_levels.push_back(
          parse_level(value_after(index, argc, argv, argument)));
    } else {
      throw std::invalid_argument("unknown option: " + std::string(argument));
    }
  }

  if (!options.help) {
    if (options.trace.empty())
      throw std::invalid_argument("--trace is required");
    if (options.output_dir.empty())
      throw std::invalid_argument("--output-dir is required");
    if (options.cache_name.empty())
      throw std::invalid_argument("--cache-name must not be empty");
    if (options.replacement_policy != "lru" &&
        options.replacement_policy != "fifo" &&
        options.replacement_policy != "random")
      throw std::invalid_argument(
          "--replacement-policy must be lru, fifo or random");
    for (std::size_t index = 0; index < options.cache_levels.size(); ++index)
      for (std::size_t previous = 0; previous < index; ++previous)
        if (options.cache_levels[index].name ==
            options.cache_levels[previous].name)
          throw std::invalid_argument("duplicate --cache-level name: " +
                                      options.cache_levels[index].name);
  }
  return options;
}

cache_simulator::CacheLevel make_level(const LevelOptions &options) {
  using cache_simulator::CacheLevel;
  using cache_simulator::CacheLevelConfig;
  using cache_simulator::FIFO;
  using cache_simulator::LRU;
  using cache_simulator::Random;
  using cache_simulator::ReplacementPolicyFactory;
  using cache_simulator::SetAssociativeCacheLevel;

  ReplacementPolicyFactory policy_factory;
  std::string policy_name;
  if (options.replacement_policy == "lru") {
    policy_factory = [] { return std::make_unique<LRU>(); };
    policy_name = "LRU";
  } else if (options.replacement_policy == "fifo") {
    policy_factory = [] { return std::make_unique<FIFO>(); };
    policy_name = "FIFO";
  } else {
    policy_factory = [seed = options.random_seed] {
      return std::make_unique<Random>(seed);
    };
    policy_name = "Random(seed=" + std::to_string(options.random_seed) + ")";
  }
  auto implementation = std::make_unique<SetAssociativeCacheLevel>(
      CacheLevelConfig{options.size_bytes, options.line_size_bytes,
                       options.associativity},
      policy_factory);
  return {options.name,
          {options.size_bytes, options.line_size_bytes, options.associativity,
           policy_name},
          std::move(implementation)};
}

cache_simulator::CacheHierarchy make_hierarchy(const Options &options) {
  using cache_simulator::CacheLevel;

  std::vector<CacheLevel> levels;
  if (options.cache_levels.empty()) {
    levels.push_back(
        make_level({options.cache_name, options.size_bytes,
                    options.line_size_bytes, options.associativity,
                    options.replacement_policy, options.random_seed}));
  } else {
    levels.reserve(options.cache_levels.size());
    for (const auto &level : options.cache_levels)
      levels.push_back(make_level(level));
  }
  return cache_simulator::CacheHierarchy(std::move(levels));
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto options = parse_options(argc, argv);
    if (options.help) {
      print_usage(std::cout);
      return 0;
    }

    fs::create_directories(options.output_dir);
    auto hierarchy = make_hierarchy(options);
    const auto statistics_path = options.output_dir / "statistics.json";
    const auto summary =
        cache_simulator::run_trace(options.trace, hierarchy, statistics_path);

    std::cout << "CacheScope replay complete\n"
              << "trace:      " << fs::absolute(options.trace) << '\n'
              << "levels:     ";
    const auto names = hierarchy.level_names();
    for (std::size_t index = 0; index < names.size(); ++index) {
      if (index != 0)
        std::cout << " -> ";
      const auto &config = hierarchy.configuration(index + 1);
      std::cout << names[index] << '(' << config.size_bytes << " B, "
                << config.associativity << "-way, " << config.replacement_policy
                << ')';
    }
    std::cout << '\n'
              << "events:     " << summary.events << '\n'
              << "statistics: " << fs::absolute(statistics_path) << '\n'
              << "timeline:   "
              << fs::absolute(options.output_dir / "events.json") << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "cachescope-runner: " << error.what() << '\n';
    print_usage(std::cerr);
    return 1;
  }
}
