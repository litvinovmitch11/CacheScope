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

static LineAccessResult line(U64 address, bool hit, std::size_t level,
                             std::optional<U64> evicted = std::nullopt) {
  return {address, hit, 0, 0, evicted, level};
}

namespace {
class ScriptedCache : public ICacheSimulator {
public:
  std::vector<std::string> names{"L1", "L2", "L3"};
  std::vector<Call> calls;
  unsigned clears = 0;
  std::function<AccessResult(const Call &, std::size_t)> response =
      [](const Call &call, std::size_t) {
        return AccessResult{{line(call.address / 64 * 64, true, 1)}};
      };

  std::vector<std::string> level_names() const override { return names; }
  void clear() override {
    ++clears;
    calls.clear();
  }
  AccessResult read(U64 address, std::size_t size) override {
    return access({address, size, false});
  }
  AccessResult write(U64 address, std::size_t size) override {
    return access({address, size, true});
  }

private:
  AccessResult access(Call call) {
    calls.push_back(call);
    return response(call, calls.size() - 1);
  }
};

class RandomMissCache : public ICacheSimulator {
public:
  U64 hits = 0;
  U64 misses = 0;
  unsigned clears = 0;

  std::vector<std::string> level_names() const override { return {"L1"}; }
  void clear() override {
    ++clears;
    hits = misses = 0;
    generator_.seed(42);
    miss_.reset();
  }
  AccessResult read(U64 address, std::size_t size) override {
    return access(address, size);
  }
  AccessResult write(U64 address, std::size_t size) override {
    return access(address, size);
  }

private:
  AccessResult access(U64 address, std::size_t size) {
    check(size == 4 && address % 64 <= 60, "smoke fixture must touch one line");
    const bool hit = !miss_(generator_);
    hit ? ++hits : ++misses;
    return {{{address / 64 * 64, hit, 0, 0, std::nullopt, 1}}};
  }

  std::mt19937 generator_{42};
  std::bernoulli_distribution miss_{0.5};
};
} // namespace

static void integration() {
  Directory directory;
  const auto input = directory.path / "trace.json";
  const auto output = directory.path / "report.json";
  write_text(input, fixture().dump());
  ScriptedCache cache;
  cache.response = [](const Call &call, std::size_t index) {
    if (index == 0) {
      check(!call.write && call.size == 8 && call.address == 0x1000003c,
            "cross-line read dispatch");
      auto second = line(0x10000040, false, 1, 0);
      second.set = 1;
      second.way = 1;
      return AccessResult{
          {line(0x10000000, true, 1), second, line(0x10000040, true, 2)}};
    }
    if (index == 1) {
      check(call.write && call.size == 4 && call.address == 0x10000080,
            "write dispatch and non-overlapping placement");
      return AccessResult{{line(call.address, false, 1),
                           line(call.address, false, 2),
                           line(call.address, true, 3)}};
    }
    check(!call.write && call.size == 4 && call.address == 0x10000000,
          "repeated instruction dispatch");
    return AccessResult{{line(call.address, true, 1)}};
  };
  const auto summary = run_trace(input, cache, output);
  check(cache.clears == 1 && cache.calls.size() == 3, "lifecycle");
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
  check(report["schema"] == "cachescope.statistics.v1", "report schema");
  check(report["events"].size() == 3, "every event recorded");
  check(report["events"][0]["event"] == fixture()["events"][0],
        "original event retained");
  check(report["events"][0]["lines"][1]["set"] == 1 &&
            report["events"][0]["lines"][1]["way"] == 1,
        "set and way retained");
  check(report["instructions"][0]["access_id"] == 10 &&
            report["instructions"][0]["levels"][2]["total"]["hit_rate_percent"]
                .is_null(),
        "sorted instructions and null rates");
  const auto golden =
      fs::path(CACHESCOPE_FIXTURE_DIR) / "example.statistics.json";
  check(report == read_json(golden), "versioned report fixture");
  const auto original_report = read_text(output);
  run_trace(input, cache, output);
  check(read_text(output) == original_report && cache.clears == 2,
        "byte-identical repeated runs");

  auto negative = single_event();
  negative["events"][0]["byte_offset"] = -4;
  negative["events"][0]["size_bytes"] = 8;
  negative["events"][0]["extra"] = {
      {"nested", Json::array({1, Json{{"x", 2}}})}};
  negative["future_root"] = {{"large", Json::array({1, 2, 3})}};
  write_text(input, negative.dump());
  ScriptedCache negative_cache;
  const auto negative_summary = run_trace(input, negative_cache, output);
  check(negative_summary.buffers.at(0).base_address == 0x10000040 &&
            negative_cache.calls[0].address == 0x1000003c,
        "negative offsets preserve aligned logical origin");
  check(read_json(output)["events"][0]["event"]["extra"] ==
            negative["events"][0]["extra"],
        "unknown event fields retained");

  negative["events"][0]["byte_offset"] =
      std::numeric_limits<std::int64_t>::min();
  negative["events"][0]["size_bytes"] = 1;
  write_text(input, negative.dump());
  run_trace(input, negative_cache, output);
  check(negative_cache.calls[0].address == 0x10000000,
        "INT64_MIN offset without signed overflow");

  auto empty = fixture();
  empty["events"] = Json::array();
  empty["total_events"] = empty["recorded_events"] = 0;
  write_text(input, empty.dump());
  ScriptedCache empty_cache;
  const auto empty_summary = run_trace(input, empty_cache, output);
  check(empty_cache.clears == 1 && empty_cache.calls.empty(),
        "empty trace lifecycle");
  check(empty_summary.instructions.size() == 2,
        "unexecuted access sites retained");
  check(read_json(output)["summary"]["levels"][2]["total"]["hit_rate_percent"]
            .is_null(),
        "empty level rate is null");
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
    ScriptedCache cache;
    expect_error([&] { run_trace(input, cache, output); }, message);
    check(cache.clears == 0 && cache.calls.empty(),
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
  mutate([](Json &j) { j["events"][0].erase("indices"); },
         "missing field: indices");
  mutate([](Json &j) { j["events"][0]["indices"] = 1; },
         "indices must be an array");
  mutate([](Json &j) { j["events"][0]["indices"][0] = 0.5; },
         "signed 64-bit integer");
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
  ScriptedCache same_path;
  expect_error([&] { run_trace(input, same_path, input); }, "must differ");
  check(read_json(input) == fixture(), "input not overwritten");
  expect_error(
      [&] { run_trace(directory.path / "missing", same_path, output); },
      "cannot open");

  const auto bad_model =
      [&](const std::function<void(ScriptedCache &)> &configure,
          const std::string &message) {
        ScriptedCache cache;
        configure(cache);
        write_text(output, old_report);
        expect_error([&] { run_trace(input, cache, output); }, message);
        check(read_text(output) == old_report,
              "model failure preserves old report");
        for (const auto &entry : fs::directory_iterator(directory.path))
          check(entry.path().filename().string().find(".tmp.") ==
                    std::string::npos,
                "temporary report cleaned up");
      };
  bad_model([](ScriptedCache &c) { c.names.clear(); }, "nonempty level names");
  bad_model([](ScriptedCache &c) { c.names = {""}; }, "nonempty level names");
  bad_model(
      [](ScriptedCache &c) {
        c.response = [](const Call &, std::size_t) { return AccessResult{}; };
      },
      "no demand probes");
  for (const auto level : {0U, 4U})
    bad_model(
        [&](ScriptedCache &c) {
          c.response = [=](const Call &call, std::size_t) {
            return AccessResult{{line(call.address, true, level)}};
          };
        },
        "invalid level");
  bad_model(
      [](ScriptedCache &c) {
        c.response = [](const Call &call, std::size_t) {
          return AccessResult{{line(call.address, true, 2)}};
        };
      },
      "omitted first-level");
  bad_model(
      [](ScriptedCache &c) {
        c.response = [](const Call &call, std::size_t index) {
          if (index == 1)
            throw std::runtime_error("injected failure");
          return AccessResult{{line(call.address, true, 1)}};
        };
      },
      "event 1: injected failure");
  ScriptedCache io_cache;
  const auto blocked = directory.path / "not-a-directory";
  write_text(blocked, "file");
  expect_error([&] { run_trace(input, io_cache, blocked / "report.json"); },
               "trace ");
  check(io_cache.clears == 0, "output-open failure must not clear cache");
}

static void generated_trace(const fs::path &path, U64 count, bool mixed) {
  auto trace = single_event();
  trace["total_events"] = trace["recorded_events"] = count;
  if (mixed)
    trace["accesses"].push_back(
        {{"access_id", 20}, {"memref_id", 1}, {"kind", "write"}});
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
           << "\",\"indices\":[0,0],\"address_kind\":\"memref_offset\"}";
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
  generated_trace(input, 1000, true);
  RandomMissCache cache;
  const auto summary = run_trace(input, cache, output);
  const auto report = read_json(output);
  check(cache.clears == 1 && cache.hits > 0 && cache.misses > 0,
        "random model exercised");
  check(cache.hits + cache.misses == 1000, "one outcome per smoke access");
  check(summary.levels[0].total.hits == cache.hits &&
            summary.levels[0].total.misses == cache.misses,
        "summary matches actual random outcomes");
  U64 observed_hits = 0;
  for (const auto &entry : report["events"])
    observed_hits += entry["lines"][0]["hit"].get<bool>() ? 1 : 0;
  check(observed_hits == cache.hits && report["events"].size() == 1000,
        "random outcomes retained in report");
  check(report["summary"]["levels"][0]["total"]["misses"] == cache.misses &&
            report["summary"]["levels"][0]["total"]["hit_rate_percent"] ==
                *summary.levels[0].total.hit_rate_percent(),
        "random report matches summary");
  check(summary.instructions.at(10).events == 500 &&
            summary.instructions.at(20).events == 500,
        "both read and write paths exercised");
  const auto previous = read_text(output);
  run_trace(input, cache, output);
  check(previous == read_text(output),
        "fixed seed reset makes smoke repeatable");
  std::cout << "Smoke report: " << output << "\n"
            << "Hits: " << cache.hits << ", misses: " << cache.misses << "\n";
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
  bool start_object(std::size_t) override { return true; }
  bool key(string_t &key) override {
    if (key == "sequence")
      ++count;
    return true;
  }
  bool end_object() override { return true; }
  bool start_array(std::size_t) override { return true; }
  bool end_array() override { return true; }
  bool parse_error(std::size_t, const std::string &,
                   const nlohmann::detail::exception &) override {
    return false;
  }
};
} // namespace

static void streaming() {
  Directory directory;
  const auto input = directory.path / "trace.json";
  const auto output = directory.path / "report.json";
  constexpr U64 count = 1000001;
  generated_trace(input, count, false);
  RandomMissCache cache;
  const auto summary = run_trace(input, cache, output);
  check(summary.events == count && cache.hits + cache.misses == count,
        "no producer-sized replay limit");
  check(summary.instructions.size() == 1 && summary.buffers.size() == 1,
        "retained state depends on static sites, not trace length");
  SequenceCounter counter;
  std::ifstream report(output);
  check(Json::sax_parse(report, &counter) && counter.count == count,
        "complete large report parsed without DOM");
}

} // namespace runner_tests

int main(int argc, char **argv) {
  using namespace runner_tests;
  try {
    check(argc == 2, "expected suite name");
    const std::string suite = argv[1];
    if (suite == "integration")
      integration();
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
