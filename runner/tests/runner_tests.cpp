#include "cache_simulator/FIFO.h"
#include "cache_simulator/LRU.h"
#include "cache_simulator/Random.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include "cache_simulator/trace_runner.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <unistd.h>

using namespace cache_simulator;
using Json = nlohmann::json;
using U64 = std::uint64_t;
namespace fs = std::filesystem;

namespace runner_tests {

static void check(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

namespace {
class Directory {
public:
  Directory() {
    auto pattern =
        (fs::temp_directory_path() / "cachescope-runner-test-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    auto *const result = mkdtemp(name.data());
    check(result != nullptr, "create test directory");
    path = result;
  }

  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }

  fs::path path;
};
} // namespace

static void write_text(const fs::path &path, const std::string &text) {
  std::ofstream output(path);
  output << text;
  output.close();
  check(!output.fail(), "write fixture");
}

static std::string read_text(const fs::path &path) {
  std::ifstream input(path);
  check(input.good(), "read file");
  return {std::istreambuf_iterator<char>(input), {}};
}

static Json read_json(const fs::path &path) {
  return Json::parse(read_text(path));
}

static Json fixture() {
  return read_json(fs::path(CACHESCOPE_FIXTURE_DIR) / "example.trace.json");
}

static Json single_event() {
  auto trace = fixture();
  trace["events"] = Json::array({trace["events"][2]});
  trace["events"][0]["sequence"] = 0;
  trace["accesses"] = Json::array({trace["accesses"][1]});
  trace["total_events"] = 1;
  trace["recorded_events"] = 1;
  return trace;
}

namespace {
struct Call {
  U64 address;
  std::size_t size;
  bool write;
};
} // namespace

namespace {
class ScriptedLevel : public ICacheLevel {
public:
  mutable std::vector<Call> calls;
  std::vector<Call> touches;
  std::vector<CacheLine> inserts;
  unsigned clears = 0;
  std::function<LookupResult(U64, std::size_t)> on_lookup =
      [](U64, std::size_t) { return LookupResult{0, 0}; };
  std::function<InsertResult(CacheLine, std::size_t)> on_insert =
      [](CacheLine, std::size_t) { return InsertResult{0, 0, std::nullopt}; };
  std::function<std::optional<CacheLine>(U64)> on_invalidate = [](U64 address) {
    return CacheLine{address, false};
  };

  void clear() override {
    ++clears;
    calls.clear();
    touches.clear();
    inserts.clear();
  }
  LookupResult lookup(U64 address) const override {
    calls.push_back({address, 64, false});
    return on_lookup(address, calls.size() - 1);
  }
  void touch(U64 address, AccessType type) override {
    touches.push_back({address, 64, type == AccessType::Write});
  }
  std::optional<CacheLine> invalidate(U64 address) override {
    return on_invalidate(address);
  }
  InsertResult insert(CacheLine line) override {
    inserts.push_back(line);
    return on_insert(line, inserts.size() - 1);
  }
};

struct HierarchyFixture {
  std::vector<ScriptedLevel *> levels;
  CacheHierarchy hierarchy;

  HierarchyFixture() : hierarchy(make_levels(levels)) {}

private:
  static std::vector<CacheLevel>
  make_levels(std::vector<ScriptedLevel *> &view) {
    std::vector<CacheLevel> result;
    for (unsigned index = 0; index < 3; ++index) {
      auto model = std::make_unique<ScriptedLevel>();
      view.push_back(model.get());
      result.push_back({"L" + std::to_string(index + 1),
                        {256, 64, 2, "scripted"},
                        std::move(model)});
    }
    return result;
  }
};

// Deliberately synthetic: independent 50% outcomes, not a residency model.
class RandomMissLevel : public ICacheLevel {
public:
  mutable U64 hits = 0;
  mutable U64 misses = 0;
  unsigned clears = 0;

  void clear() override {
    ++clears;
    hits = misses = 0;
    generator_.seed(42);
    miss_.reset();
  }
  LookupResult lookup(U64 address) const override {
    check(address % 64 == 0, "level receives an aligned line");
    const bool hit = !miss_(generator_);
    hit ? ++hits : ++misses;
    return {0, hit ? std::optional<std::size_t>(0) : std::nullopt};
  }
  void touch(U64, AccessType) override {}
  std::optional<CacheLine> invalidate(U64 address) override {
    return CacheLine{address, false};
  }
  InsertResult insert(CacheLine) override { return {0, 0, std::nullopt}; }

private:
  mutable std::mt19937 generator_{42};
  mutable std::bernoulli_distribution miss_{0.5};
};

struct RandomFixture {
  RandomMissLevel *level = nullptr;
  CacheHierarchy hierarchy;

  RandomFixture() : hierarchy(make_levels(level)) {}

private:
  static std::vector<CacheLevel> make_levels(RandomMissLevel *&view) {
    auto model = std::make_unique<RandomMissLevel>();
    view = model.get();
    std::vector<CacheLevel> result;
    result.push_back({"L1", {64, 64, 1, "random-miss-test"}, std::move(model)});
    return result;
  }
};
} // namespace

static void integration() {
  Directory directory;
  const auto input = directory.path / "trace.json";
  const auto output = directory.path / "statistics.json";
  const auto events = directory.path / "events.json";
  write_text(input, fixture().dump());
  HierarchyFixture cache;
  cache.levels[0]->on_lookup = [](U64 address, std::size_t index) {
    const U64 addresses[] = {0x10000000, 0x10000040, 0x10000080, 0x10000000};
    check(index < 4 && address == addresses[index],
          "split and aligned probe order");
    const bool hit = index == 0 || index == 3;
    return LookupResult{index == 1 ? 1U : 0U,
                        hit ? std::optional<std::size_t>(0) : std::nullopt};
  };
  cache.levels[0]->on_insert = [](CacheLine line, std::size_t index) {
    if (index == 0) {
      check(line.line_address == 0x10000040 && !line.dirty,
            "promote clean read line");
      return InsertResult{1, 1, CacheLine{0, false}};
    }
    check(line.line_address == 0x10000080 && line.dirty,
          "write-allocate sets dirty");
    return InsertResult{0, 0, std::nullopt};
  };
  cache.levels[1]->on_lookup = [](U64 address, std::size_t index) {
    check(address == (index == 0 ? 0x10000040U : 0x10000080U),
          "lower-level addresses");
    return LookupResult{0, index == 0 ? std::optional<std::size_t>(0)
                                      : std::nullopt};
  };
  const auto summary = run_trace(input, cache.hierarchy, output);
  check(cache.levels[0]->clears == 1 && cache.levels[0]->calls.size() == 4,
        "hierarchy lifecycle and line splitting");
  check(cache.levels[0]->touches.size() == 2 &&
            cache.levels[1]->touches.size() == 1 &&
            cache.levels[2]->touches.size() == 1 &&
            !cache.levels[2]->touches[0].write,
        "touch only hits; write allocation does not write the lower level");
  check(summary.events == 3 && summary.reads == 2 && summary.writes == 1,
        "event counts");
  check(summary.read_bytes == 12 && summary.write_bytes == 4,
        "requested bytes");
  check(summary.levels[0].total.probes == 4 &&
            summary.levels[0].total.hits == 2 &&
            summary.levels[0].total.misses == 2 &&
            summary.levels[0].total.evictions == 1,
        "per-line totals including eviction of address zero");
  check(summary.levels[0].total.hit_rate_percent() == 50.0 &&
            summary.levels[1].total.hit_rate_percent() == 50.0 &&
            summary.levels[2].total.hit_rate_percent() == 100.0,
        "local level rates");
  const auto &instruction = summary.instructions.at(10);
  check(instruction.events == 2 && instruction.requested_bytes == 12,
        "instruction source counts");
  check(std::abs(*instruction.levels[0].total.hit_rate_percent() - 200.0 / 3) <
            1e-10,
        "instruction line rate");
  check(!instruction.levels[2].total.hit_rate_percent(),
        "unvisited instruction level");
  const auto report = read_json(output);
  const auto timeline = read_json(events);
  check(report["schema"] == "cachescope.simulation-statistics.v1" &&
            timeline["schema"] == "cachescope.simulation-events.v1",
        "UI schemas");
  check(report["producer"] == "CacheScope" &&
            report["source"]["producer"] == fixture()["producer"],
        "UI producer stays a string; full producer metadata retained");
  check(timeline["events"].size() == 3 && !report.contains("events"),
        "summary separated from timeline");
  check(timeline["events"][0]["trace_event"] == fixture()["events"][0],
        "original event retained");
  check(timeline["events"][0]["address"] == "0x1000003c" &&
            timeline["events"][0]["lines"][1]["evicted_address"] == "0x0",
        "addresses serialized as lossless hex strings");
  check(timeline["events"][0]["lines"][1]["set"] == 1 &&
            timeline["events"][0]["lines"][1]["way"] == 1,
        "set/way retained");
  check(timeline["events"][1]["level_results"][1]["lines"][0]["way"].is_null(),
        "exclusive lower-level miss does not allocate");
  check(report["instructions"][0]["access_id"] == 10 &&
            report["instructions"][0]["levels"][2]["totals"]["hit_rate_percent"]
                .is_null(),
        "sorted instructions and null rates");
  check(report == read_json(fs::path(CACHESCOPE_FIXTURE_DIR) /
                            "example.statistics.json"),
        "versioned statistics fixture");
  check(timeline ==
            read_json(fs::path(CACHESCOPE_FIXTURE_DIR) / "example.events.json"),
        "versioned event fixture");
  const auto original_report = read_text(output);
  const auto original_events = read_text(events);
  run_trace(input, cache.hierarchy, output);
  check(read_text(output) == original_report &&
            read_text(events) == original_events &&
            cache.levels[0]->clears == 2,
        "byte-identical repeated runs");

  auto negative = single_event();
  negative["events"][0]["byte_offset"] = -4;
  negative["events"][0]["size_bytes"] = 8;
  negative["events"][0]["extra"] = {
      {"nested", Json::array({1, Json{{"x", 2}}})}};
  negative["events"][0]["indices"] = "ignored legacy extension";
  negative["future_root"] = {{"large", Json::array({1, 2, 3})}};
  write_text(input, negative.dump());
  HierarchyFixture negative_cache;
  const auto negative_summary =
      run_trace(input, negative_cache.hierarchy, output);
  check(negative_summary.buffers.at(0).base_address == 0x10000040 &&
            read_json(events)["events"][0]["address"] == "0x1000003c",
        "negative offsets preserve aligned logical origin");
  check(read_json(events)["events"][0]["trace_event"]["extra"] ==
            negative["events"][0]["extra"],
        "unknown event fields retained");
  negative["events"][0]["byte_offset"] =
      std::numeric_limits<std::int64_t>::min();
  negative["events"][0]["size_bytes"] = 1;
  write_text(input, negative.dump());
  run_trace(input, negative_cache.hierarchy, output);
  check(negative_cache.levels[0]->calls[0].address == 0x10000000,
        "INT64_MIN offset without signed overflow");

  auto empty = fixture();
  empty["events"] = Json::array();
  empty["total_events"] = empty["recorded_events"] = 0;
  write_text(input, empty.dump());
  HierarchyFixture empty_cache;
  const auto empty_summary = run_trace(input, empty_cache.hierarchy, output);
  check(empty_cache.levels[0]->clears == 1 &&
            empty_cache.levels[0]->calls.empty(),
        "empty trace lifecycle");
  check(empty_summary.instructions.size() == 2,
        "unexecuted access sites retained");
  check(read_json(output)["levels"][2]["totals"]["hit_rate_percent"].is_null(),
        "empty level rate is null");
}

static void real_cache() {
  Directory directory;
  const auto input = directory.path / "trace.json";
  const auto output = directory.path / "statistics.json";
  std::vector<CacheLevel> levels;
  for (std::size_t lines : {1, 2})
    levels.push_back({"L" + std::to_string(lines),
                      {64 * lines, 64, lines, "LRU"},
                      std::make_unique<SetAssociativeCacheLevel>(
                          CacheLevelConfig{64 * lines, 64, lines},
                          [] { return std::make_unique<LRU>(); })});
  CacheHierarchy hierarchy(std::move(levels));
  write_text(input, fixture().dump());
  const auto summary = run_trace(input, hierarchy, output);
  check(summary.events == 3 && summary.levels[0].total.hits == 0 &&
            summary.levels[0].total.misses == 4 &&
            summary.levels[1].total.hits == 1 &&
            summary.levels[1].total.misses == 3 && summary.memory_reads == 3,
        "real LRU cache: cold split accesses and exclusive L2 promotion");
  const auto report = read_json(output);
  check(report["totals"] == report["levels"][0]["totals"] &&
            report["processed"] == report["levels"][0]["processed"] &&
            report["cache_config"] == report["levels"][0]["cache_config"] &&
            report["levels"][1]["totals"]["hit_rate_percent"] == 25.0,
        "UI root is an exact L1 projection with local rates for lower levels");

  const fs::path upstream = CACHESCOPE_TRACE_FIXTURE_DIR;
  for (const auto &name :
       {"trace-v1-complete.json", "source-map-v1/trace.json"}) {
    const auto trace_path = upstream / name;
    const auto trace = read_json(trace_path);
    const auto result = run_trace(trace_path, hierarchy, output);
    const auto statistics = read_json(output);
    const auto timeline = read_json(directory.path / "events.json");
    check(result.events == 1 && result.memory_reads == 1 &&
              statistics["producer"] == trace["producer"]["name"] &&
              statistics["source"]["producer"] == trace["producer"] &&
              timeline["events"][0]["trace_event"] == trace["events"][0],
          "current compiler fixture replays without indices or metadata loss");
    for (const auto &field : trace["accesses"][0].items())
      check(statistics["instructions"][0][field.key()] == field.value(),
            "static access metadata retained for UI source navigation");
    if (trace.contains("source_map"))
      check(statistics["source"]["source_map"] == trace["source_map"],
            "embedded source map retained");
  }
}

static Json replacement_trace() {
  auto trace = fixture();
  for (auto &access : trace["accesses"])
    access["memref_id"] = 0;
  trace["events"] = Json::array();
  for (const U64 offset : {0, 64, 0, 128, 0, 192, 256, 320}) {
    const auto sequence = trace["events"].size();
    trace["events"].push_back({{"sequence", sequence},
                               {"access_id", sequence == 0 ? 20 : 10},
                               {"memref_id", 0},
                               {"byte_offset", offset},
                               {"size_bytes", 4},
                               {"kind", sequence == 0 ? "write" : "read"},
                               {"address_kind", "memref_offset"}});
  }
  trace["total_events"] = trace["recorded_events"] = trace["events"].size();
  return trace;
}

static CacheHierarchy
replacement_hierarchy(const std::string &policy,
                      const ReplacementPolicyFactory &factory) {
  std::vector<CacheLevel> levels;
  for (const std::size_t ways : {2, 1})
    levels.push_back({"L" + std::to_string(levels.size() + 1),
                      {64 * ways, 64, ways, policy},
                      std::make_unique<SetAssociativeCacheLevel>(
                          CacheLevelConfig{64 * ways, 64, ways}, factory)});
  return CacheHierarchy(std::move(levels));
}

static void fifo_cache() {
  Directory directory;
  const auto input = directory.path / "trace.json";
  const auto output = directory.path / "statistics.json";
  write_text(input, replacement_trace().dump());
  auto hierarchy =
      replacement_hierarchy("FIFO", [] { return std::make_unique<FIFO>(); });
  const auto summary = run_trace(input, hierarchy, output);
  check(summary.events == 8 && summary.reads == 7 && summary.writes == 1 &&
            summary.levels[0].total.hits == 1 &&
            summary.levels[0].total.misses == 7 &&
            summary.levels[1].total.hits == 1 &&
            summary.levels[1].total.misses == 6,
        "FIFO preserves insertion order through runner and hierarchy");
  check(
      summary.memory_reads == 6 && summary.memory_writebacks == 1 &&
          summary.levels[0].total.evictions == 5 &&
          summary.levels[1].total.evictions == 3,
      "FIFO cascades victims and writes the dirty last-level victim to memory");
  check(summary.instructions.at(10).events == 7 &&
            summary.instructions.at(10).levels[0].total.hits == 1 &&
            summary.instructions.at(20).events == 1 &&
            summary.instructions.at(20).levels[0].total.misses == 1,
        "FIFO instruction statistics distinguish reads and writes");
  const auto report = read_json(output);
  const auto events = read_json(directory.path / "events.json")["events"];
  check(
      report["levels"].size() == 2 &&
          report["levels"][0]["cache_config"]["replacement_policy"] == "FIFO" &&
          report["levels"][1]["cache_config"]["replacement_policy"] == "FIFO" &&
          report["totals"]["hit_rate_percent"] == 12.5 &&
          report["memory"]["writebacks"] == 1 && events.size() == 8,
      "FIFO reports contain both levels, their statistics and all events");
  check(events[3]["lines"][0]["evicted_address"] == "0x10000000" &&
            events[3]["lines"][0]["evicted_dirty"] == true &&
            events[4]["level_results"][1]["lines"][0]["hit"] == true &&
            events[4]["memory_reads"] == 0 &&
            events[7]["evictions"].back()["line_address"] == "0x10000000" &&
            events[7]["evictions"].back()["dirty"] == true &&
            events[7]["memory_writebacks"] == 1,
        "FIFO timeline retains dirty through L2 promotion and final writeback");
}

static void random_cache() {
  Directory directory;
  const auto input = directory.path / "trace.json";
  const auto output = directory.path / "statistics.json";
  const auto events_path = directory.path / "events.json";
  write_text(input, replacement_trace().dump());
  auto hierarchy = replacement_hierarchy(
      "Random", [] { return std::make_unique<Random>(42); });
  const auto summary = run_trace(input, hierarchy, output);
  const auto l1_hits = summary.levels[0].total.hits;
  check(summary.events == 8 && summary.reads == 7 && summary.writes == 1 &&
            summary.levels[0].total.probes == 8 && l1_hits >= 1 &&
            l1_hits <= 2 && summary.levels[1].total.hits == 2 - l1_hits &&
            summary.levels[1].total.misses == 6 && summary.memory_reads == 6 &&
            summary.memory_writebacks <= 1,
        "Random hierarchy serves two resident accesses and six cold lines");
  const auto report = read_json(output);
  const auto events = read_json(events_path)["events"];
  check(report["levels"].size() == 2 &&
            report["levels"][0]["cache_config"]["replacement_policy"] ==
                "Random" &&
            report["levels"][1]["cache_config"]["replacement_policy"] ==
                "Random" &&
            report["memory"]["reads"] == 6 &&
            report["instructions"][0]["events"] == 7 &&
            report["instructions"][0]["levels"][0]["totals"]["hits"] ==
                l1_hits &&
            report["instructions"][1]["events"] == 1 && events.size() == 8,
        "Random reports retain per-level and per-instruction statistics");
  const auto &victim = events[3]["lines"][0]["evicted_address"];
  check((victim == "0x10000000" || victim == "0x10000040") &&
            events[3]["lines"][0]["evicted_dirty"] ==
                (victim == "0x10000000") &&
            events[2]["lines"][0]["hit"] == true &&
            events[4]["level_results"].back()["lines"][0]["hit"] == true &&
            events[4]["memory_reads"] == 0,
        "Random evicts a resident line and retains the later hierarchy hit");
  const auto previous_statistics = read_text(output);
  const auto previous_events = read_text(events_path);
  for (unsigned repetition = 0; repetition < 4; ++repetition) {
    run_trace(input, hierarchy, output);
    check(read_text(output) == previous_statistics &&
              read_text(events_path) == previous_events,
          "fixed Random seed resets with hierarchy on every runner invocation");
  }
}

template <class Action>
static void expect_error(Action action, const std::string &message) {
  try {
    action();
  } catch (const std::runtime_error &error) {
    check(std::string(error.what()).find(message) != std::string::npos,
          "expected error '" + message + "', got '" + error.what() + "'");
    return;
  }
  throw std::runtime_error("expected failure: " + message);
}

static void validation() {
  Directory directory;
  const auto input = directory.path / "trace.json";
  const auto output = directory.path / "report.json";
  const std::string old_report = "previous report";
  const auto bad_trace = [&](const std::string &text,
                             const std::string &message) {
    write_text(input, text);
    write_text(output, old_report);
    HierarchyFixture cache;
    expect_error([&] { run_trace(input, cache.hierarchy, output); }, message);
    check(cache.levels[0]->clears == 0 && cache.levels[0]->calls.empty(),
          "bad input must not mutate cache");
    check(read_text(output) == old_report, "bad input preserves report");
  };
  const auto mutate = [&](const std::function<void(Json &)> &change,
                          const std::string &message) {
    auto trace = single_event();
    change(trace);
    bad_trace(trace.dump(), message);
  };
  mutate([](Json &j) { j["schema"] = "cachescope.trace.v2"; }, "schema");
  mutate([](Json &j) { j["schema"] = 1; }, "schema must be a string");
  mutate([](Json &j) { j["producer"] = "legacy-producer"; },
         "producer must be an object");
  mutate([](Json &j) { j["producer"].erase("pipeline_fingerprint"); },
         "pipeline_fingerprint");
  mutate([](Json &j) { j["producer"]["llvm_revision"] = 1; },
         "llvm_revision must be a string");
  mutate([](Json &j) { j["producer"]["name"] = "Other"; }, "producer name");
  mutate([](Json &j) { j["valid"] = false; }, "invalid traces");
  mutate([](Json &j) { j["complete"] = false; }, "incomplete traces");
  mutate([](Json &j) { j["valid"] = 1; }, "boolean");
  mutate([](Json &j) { j.erase("complete"); }, "missing root field");
  mutate([](Json &j) { j["errors"] = {"ABI error"}; }, "invalid traces");
  mutate([](Json &j) { j["errors"] = Json::array({1}); }, "array of strings");
  mutate([](Json &j) { j["errors"] = nullptr; }, "array of strings");
  mutate([](Json &j) { j["source_map"] = nullptr; },
         "source_map must be an object");
  mutate([](Json &j) { j["truncated"] = true; }, "truncated");
  mutate([](Json &j) { j["total_events"] = 2; }, "event counts");
  mutate([](Json &j) { j["recorded_events"] = 2; }, "event counts");
  mutate([](Json &j) { j["truncated"] = 0; }, "boolean");
  mutate([](Json &j) { j.erase("events"); }, "missing root field");
  mutate([](Json &j) { j["events"] = Json::object(); }, "invalid object");
  mutate([](Json &j) { j["accesses"] = nullptr; }, "must be an array");
  mutate([](Json &j) { j["events"][0] = 42; }, "entries must be objects");
  mutate([](Json &j) { j["events"][0] = Json::array(); },
         "entries must be objects");
  mutate([](Json &j) { j["events"][0]["size_bytes"] = 0; }, "positive");
  mutate([](Json &j) { j["events"][0]["size_bytes"] = -1; },
         "nonnegative integer");
  mutate([](Json &j) { j["events"][0]["size_bytes"] = 4.0; },
         "nonnegative integer");
  mutate([](Json &j) { j["events"][0]["sequence"] = 1; }, "sequence");
  mutate([](Json &j) { j["events"][0]["address_kind"] = "process_address"; },
         "address_kind");
  mutate([](Json &j) { j["events"][0]["kind"] = "read_write"; },
         "unsupported access kind");
  mutate([](Json &j) { j["events"][0]["memref_id"] = -1; }, "memref_id");
  mutate([](Json &j) { j["accesses"][0].erase("source_location"); },
         "source_location");
  mutate([](Json &j) { j["accesses"][0]["address_kind"] = "process_address"; },
         "address_kind");
  mutate([](Json &j) { j["accesses"] = Json::array(); }, "accesses table");
  mutate([](Json &j) { j["accesses"].push_back(j["accesses"][0]); },
         "duplicate access_id");
  mutate([](Json &j) { j["accesses"][0]["memref_id"] = 1; }, "accesses table");
  mutate(
      [](Json &j) {
        j["events"][0]["byte_offset"] = std::numeric_limits<U64>::max();
      },
      "signed 64-bit integer");
  mutate(
      [](Json &j) {
        j["events"][0]["size_bytes"] = std::numeric_limits<U64>::max();
      },
      "overflow");
  mutate(
      [](Json &j) {
        j["events"][0]["byte_offset"] =
            std::numeric_limits<std::int64_t>::max();
        j["events"][0]["size_bytes"] = std::numeric_limits<U64>::max();
      },
      "overflow");
  bad_trace("{", "invalid JSON");
  bad_trace("[]", "root must be an object");
  bad_trace("{\"schema\":1,\"schema\":2}", "duplicate root field");
  bad_trace("{\"events\":[{\"sequence\":0,\"sequence\":1}]}",
            "duplicate record field");
  auto duplicate_sequence = fixture();
  duplicate_sequence["events"][1]["sequence"] = 0;
  bad_trace(duplicate_sequence.dump(), "sequence");
  auto changed_site = fixture();
  changed_site["events"][2]["kind"] = "write";
  bad_trace(changed_site.dump(), "changes memref_id/kind");

  write_text(input, fixture().dump());
  HierarchyFixture same_path;
  expect_error([&] { run_trace(input, same_path.hierarchy, input); },
               "must differ");
  check(read_json(input) == fixture(), "input not overwritten");
  expect_error(
      [&] {
        run_trace(directory.path / "missing", same_path.hierarchy, output);
      },
      "cannot open");

  const auto old_events = directory.path / "events.json";
  const auto bad_model =
      [&](const std::function<void(HierarchyFixture &)> &configure,
          const std::string &message) {
        HierarchyFixture cache;
        configure(cache);
        write_text(output, old_report);
        write_text(old_events, "previous events");
        expect_error([&] { run_trace(input, cache.hierarchy, output); },
                     message);
        check(read_text(output) == old_report &&
                  read_text(old_events) == "previous events",
              "model failure preserves both reports");
        for (const auto &entry : fs::directory_iterator(directory.path))
          check(entry.path().filename().string().find(".tmp.") ==
                    std::string::npos,
                "temporary report cleaned up");
      };
  bad_model(
      [](HierarchyFixture &c) {
        c.levels[0]->on_lookup = [](U64, std::size_t) {
          return LookupResult{1000, std::nullopt};
        };
      },
      "invalid lookup");
  bad_model(
      [](HierarchyFixture &c) {
        c.levels[0]->on_lookup = [](U64, std::size_t) {
          return LookupResult{0, 2};
        };
      },
      "invalid set/way");
  bad_model(
      [](HierarchyFixture &c) {
        c.levels[0]->on_lookup = [](U64, std::size_t) {
          return LookupResult{0, std::nullopt};
        };
        c.levels[1]->on_invalidate = [](U64) {
          return std::optional<CacheLine>{};
        };
      },
      "failed to invalidate");
  bad_model(
      [](HierarchyFixture &c) {
        c.levels[0]->on_lookup = [](U64, std::size_t index) {
          if (index == 1)
            throw std::runtime_error("injected failure");
          return LookupResult{0, 0};
        };
      },
      "event 0: injected failure");
  HierarchyFixture io_cache;
  const auto blocked = directory.path / "not-a-directory";
  write_text(blocked, "file");
  expect_error(
      [&] {
        run_trace(input, io_cache.hierarchy, blocked / "statistics.json");
      },
      "trace ");
  check(io_cache.levels[0]->clears == 0,
        "output-open failure must not clear cache");
  expect_error([&] { run_trace(input, io_cache.hierarchy, old_events); },
               "must differ");
  fs::remove(old_events);
  fs::create_directory(old_events);
  expect_error([&] { run_trace(input, io_cache.hierarchy, output); },
               "regular file");
  check(io_cache.levels[0]->clears == 0 && read_text(output) == old_report,
        "invalid event destination preserves statistics before replay");
}

static void generated_trace(const fs::path &path, U64 count, bool mixed) {
  auto trace = single_event();
  trace["total_events"] = trace["recorded_events"] = count;
  if (mixed)
    trace["accesses"].push_back({{"access_id", 20},
                                 {"memref_id", 1},
                                 {"kind", "write"},
                                 {"address_kind", "memref_offset"},
                                 {"source_location", "loc(unknown)"}});
  trace.erase("events");
  auto header = trace.dump();
  header.pop_back();
  std::ofstream output(path);
  output << header << ",\"events\":[";
  for (U64 index = 0; index < count; ++index) {
    if (index)
      output << ',';
    const bool write = mixed && index % 2;
    output << "{\"sequence\":" << index
           << ",\"access_id\":" << (write ? 20 : 10)
           << ",\"memref_id\":" << (write ? 1 : 0)
           << ",\"byte_offset\":0,\"size_bytes\":4,\"kind\":\""
           << (write ? "write" : "read")
           << "\",\"address_kind\":\"memref_offset\"}";
  }
  output << "]}";
  output.close();
  check(!output.fail(), "generate trace");
}

static void random_miss() {
  const fs::path directory = CACHESCOPE_SMOKE_OUTPUT_DIR;
  fs::create_directories(directory);
  const auto input = directory / "trace.json";
  const auto output = directory / "statistics.json";
  const auto events = directory / "events.json";
  generated_trace(input, 1000, true);
  RandomFixture cache;
  const auto summary = run_trace(input, cache.hierarchy, output);
  const auto report = read_json(output);
  const auto timeline = read_json(events);
  check(cache.level->clears == 1 && cache.level->hits > 0 &&
            cache.level->misses > 0,
        "random model exercised");
  check(cache.level->hits + cache.level->misses == 1000,
        "one outcome per smoke access");
  check(summary.levels[0].total.hits == cache.level->hits &&
            summary.levels[0].total.misses == cache.level->misses,
        "summary matches actual random outcomes");
  U64 observed_hits = 0;
  for (const auto &entry : timeline["events"])
    observed_hits += entry["lines"][0]["hit"].get<bool>() ? 1 : 0;
  check(observed_hits == cache.level->hits && timeline["events"].size() == 1000,
        "random outcomes retained in report");
  check(report["totals"]["misses"] == cache.level->misses &&
            report["totals"]["hit_rate_percent"] ==
                *summary.levels[0].total.hit_rate_percent(),
        "UI report matches summary");
  check(summary.instructions.at(10).events == 500 &&
            summary.instructions.at(20).events == 500,
        "both read and write paths exercised");
  const auto previous = read_text(output);
  const auto previous_events = read_text(events);
  run_trace(input, cache.hierarchy, output);
  check(previous == read_text(output) && previous_events == read_text(events),
        "fixed seed reset makes smoke repeatable");
  std::cout << "Smoke report: " << output << "\nEvents: " << events << "\n"
            << "Hits: " << cache.level->hits
            << ", misses: " << cache.level->misses << "\n";
}

namespace {
class SequenceCounter : public nlohmann::json_sax<Json> {
public:
  U64 count = 0;
  bool null() override { return true; }
  bool boolean(bool) override { return true; }
  bool number_integer(number_integer_t) override { return true; }
  bool number_unsigned(number_unsigned_t) override { return true; }
  bool number_float(number_float_t, const string_t &) override { return true; }
  bool string(string_t &) override { return true; }
  bool binary(binary_t &) override { return false; }
  bool start_object(std::size_t) override {
    ++depth_;
    return true;
  }
  bool key(string_t &key) override {
    if (depth_ == 3 && key == "sequence")
      ++count;
    return true;
  }
  bool end_object() override {
    --depth_;
    return true;
  }
  bool start_array(std::size_t) override {
    ++depth_;
    return true;
  }
  bool end_array() override {
    --depth_;
    return true;
  }
  bool parse_error(std::size_t, const std::string &,
                   const nlohmann::detail::exception &) override {
    return false;
  }

private:
  std::size_t depth_ = 0;
};
} // namespace

static void streaming() {
  Directory directory;
  const auto input = directory.path / "trace.json";
  const auto output = directory.path / "statistics.json";
  constexpr U64 count = 1000001;
  generated_trace(input, count, false);
  RandomFixture cache;
  const auto summary = run_trace(input, cache.hierarchy, output);
  check(summary.events == count &&
            cache.level->hits + cache.level->misses == count,
        "no producer-sized replay limit");
  check(summary.instructions.size() == 1 && summary.buffers.size() == 1,
        "retained state depends on static sites, not trace length");
  SequenceCounter counter;
  std::ifstream report(directory.path / "events.json");
  check(Json::sax_parse(report, &counter) && counter.count == count,
        "complete large timeline parsed without DOM");
  check(fs::file_size(output) < 10000,
        "UI statistics do not grow with trace length");
}

} // namespace runner_tests

int main(int argc, char **argv) {
  using namespace runner_tests;
  try {
    check(argc == 2, "expected suite name");
    const std::string suite = argv[1];
    if (suite == "integration")
      integration();
    else if (suite == "real-cache")
      real_cache();
    else if (suite == "fifo-cache")
      fifo_cache();
    else if (suite == "random-cache")
      random_cache();
    else if (suite == "validation")
      validation();
    else if (suite == "random-miss")
      random_miss();
    else if (suite == "streaming")
      streaming();
    else
      throw std::runtime_error("unknown suite: " + suite);
    std::cout << suite << " passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
