#include "cache_simulator/trace_runner.hpp"
#include "cache_simulator/cache_policies.hpp"
#include "cache_simulator/trace_replay.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace cache_simulator {
namespace runner_detail {

using Json = nlohmann::json;
using U64 = std::uint64_t;
using I64 = std::int64_t;
namespace fs = std::filesystem;

[[noreturn]] static void fail(const std::string &message) {
  throw std::runtime_error(message);
}

static U64 add(U64 left, U64 right) {
  if (right > std::numeric_limits<U64>::max() - left)
    fail("integer/address overflow");
  return left + right;
}

static U64 magnitude(I64 negative) {
  return static_cast<U64>(-(negative + 1)) + 1;
}

static U64 unsigned_value(const Json &value, const std::string &name) {
  if (value.is_number_unsigned())
    return value.get<U64>();
  if (value.is_number_integer() && value.get<I64>() >= 0)
    return static_cast<U64>(value.get<I64>());
  fail(name + " must be a nonnegative integer");
}

static I64 signed_value(const Json &value, const std::string &name) {
  if (value.is_number_unsigned()) {
    const auto number = value.get<U64>();
    if (number <= static_cast<U64>(std::numeric_limits<I64>::max()))
      return static_cast<I64>(number);
  } else if (value.is_number_integer()) {
    return value.get<I64>();
  }
  fail(name + " must be a signed 64-bit integer");
}

static const Json &field(const Json &object, std::string_view name) {
  if (!object.contains(name))
    fail("missing field: " + std::string(name));
  return object.at(name);
}

static std::string string_field(const Json &object, const std::string &name) {
  const auto &value = field(object, name);
  if (!value.is_string())
    fail(name + " must be a string");
  return value.get<std::string>();
}

static std::string access_kind(const Json &object) {
  auto kind = string_field(object, "kind");
  if (kind != "read" && kind != "write")
    fail("unsupported access kind: " + kind);
  return kind;
}

namespace {
struct AccessSite {
  U64 memref_id;
  std::string kind;

  bool operator==(const AccessSite &other) const {
    return memref_id == other.memref_id && kind == other.kind;
  }
};

struct Event {
  U64 sequence;
  U64 access_id;
  AccessSite access;
  I64 offset;
  U64 size;
};
} // namespace

static AccessSite site(const Json &record) {
  if (string_field(record, "address_kind") != "memref_offset")
    fail("unsupported address_kind; expected memref_offset");
  return {unsigned_value(field(record, "memref_id"), "memref_id"),
          access_kind(record)};
}

static Event event(const Json &record) {
  Event result{unsigned_value(field(record, "sequence"), "sequence"),
               unsigned_value(field(record, "access_id"), "access_id"),
               site(record),
               signed_value(field(record, "byte_offset"), "byte_offset"),
               unsigned_value(field(record, "size_bytes"), "size_bytes")};
  if (result.size == 0 || result.size > std::numeric_limits<std::size_t>::max())
    fail("size_bytes must be positive and fit size_t");
  return result;
}

// Retain static metadata and one dynamic record at a time. Unknown root fields
// are skipped without accumulating a document-sized DOM.
namespace {
class TraceSax : public nlohmann::json_sax<Json> {
public:
  using Consumer = std::function<void(Json)>;

  TraceSax(Consumer on_access, Consumer on_event,
           std::function<bool()> cancelled = {})
      : cancelled_(std::move(cancelled)), on_access_(std::move(on_access)),
        on_event_(std::move(on_event)) {}

  Json header = Json::object();

  bool null() override { return scalar(nullptr); }
  bool boolean(bool value) override { return scalar(value); }
  bool number_integer(number_integer_t value) override { return scalar(value); }
  bool number_unsigned(number_unsigned_t value) override {
    return scalar(value);
  }
  bool number_float(number_float_t value, const string_t &) override {
    return scalar(value);
  }
  bool string(string_t &value) override { return scalar(std::move(value)); }
  bool binary(binary_t &) override { fail("binary values are not JSON"); }

  bool key(string_t &value) override {
    check_cancelled();
    if (!frames_.empty()) {
      auto &frame = frames_.back();
      if (frame.value.contains(value))
        fail("duplicate record field: " + value);
      frame.key = std::move(value);
    } else if (depth_ == 1) {
      if (known(value) && !seen_.insert(value).second)
        fail("duplicate root field: " + value);
      root_key_ = std::move(value);
    }
    return true;
  }

  bool start_object(std::size_t) override {
    check_cancelled();
    if (depth_ == 0) {
      if (root_seen_)
        fail("expected one root object");
      root_seen_ = true;
    } else if (!frames_.empty() || (depth_ == 2 && !array_.empty()) ||
               (depth_ == 1 &&
                (root_key_ == "producer" || root_key_ == "source_map"))) {
      frames_.push_back({Json::object(), {}});
    } else if (depth_ == 1 && known(root_key_)) {
      fail("invalid object value for " + root_key_);
    }
    ++depth_;
    return true;
  }

  bool end_object() override {
    check_cancelled();
    if (!frames_.empty())
      finish_frame();
    --depth_;
    return true;
  }

  bool start_array(std::size_t) override {
    check_cancelled();
    if (depth_ == 0)
      fail("trace root must be an object");
    if (!frames_.empty()) {
      frames_.push_back({Json::array(), {}});
    } else if (depth_ == 1 && known(root_key_)) {
      if (root_key_ == "errors")
        frames_.push_back({Json::array(), {}});
      else if (root_key_ != "events" && root_key_ != "accesses")
        fail("invalid array value for " + root_key_);
      else
        array_ = root_key_;
    } else if (depth_ == 2 && !array_.empty()) {
      fail(array_ + " entries must be objects");
    }
    ++depth_;
    return true;
  }

  bool end_array() override {
    check_cancelled();
    if (!frames_.empty())
      finish_frame();
    if (depth_ == 2)
      array_.clear();
    --depth_;
    return true;
  }

  bool parse_error(std::size_t position, const std::string &,
                   const nlohmann::detail::exception &error) override {
    fail("invalid JSON at byte " + std::to_string(position) + ": " +
         error.what());
  }

  void validate_header(U64 count) const {
    for (const auto *name :
         {"schema", "variant", "producer", "valid", "complete", "errors",
          "total_events", "recorded_events", "truncated", "accesses", "events"})
      if (!seen_.count(name))
        fail(std::string("missing root field: ") + name);
    if (string_field(header, "schema") != "cachescope.trace.v1")
      fail("unsupported trace schema");
    string_field(header, "variant");
    const auto &producer = field(header, "producer");
    if (!producer.is_object())
      fail("producer must be an object");
    for (const auto *name : {"name", "version", "llvm_version", "llvm_revision",
                             "pipeline_fingerprint"})
      string_field(producer, name);
    if (producer.at("name") != "CacheScope")
      fail("unexpected producer name");
    for (const auto *name : {"valid", "complete", "truncated"})
      if (!field(header, name).is_boolean())
        fail(std::string(name) + " must be boolean");
    const auto &errors = field(header, "errors");
    if (!errors.is_array())
      fail("errors must be an array of strings");
    for (const auto &error : errors)
      if (!error.is_string())
        fail("errors must be an array of strings");
    if (!header.at("valid").get<bool>() || !errors.empty())
      fail("invalid traces are not supported: valid must be true and errors "
           "empty");
    if (header.at("truncated").get<bool>())
      fail("truncated traces are not supported");
    if (!header.at("complete").get<bool>())
      fail("incomplete traces are not supported");
    const auto recorded =
        unsigned_value(field(header, "recorded_events"), "recorded_events");
    const auto total =
        unsigned_value(field(header, "total_events"), "total_events");
    if (recorded != count || total != count)
      fail("event counts do not match the complete trace");
    if (header.contains("source_map")) {
      const auto &mapping = header.at("source_map");
      if (!mapping.is_object())
        fail("source_map must be an object");
      if (string_field(mapping, "schema") != "cachescope.source-map.v1")
        fail("unsupported source_map schema");
    }
  }

private:
  void check_cancelled() const {
    if (cancelled_ && cancelled_())
      fail("preparation cancelled");
  }

  struct Frame {
    Json value;
    std::string key;
  };

  static bool known(const std::string &key) {
    return key == "schema" || key == "variant" || key == "producer" ||
           key == "total_events" || key == "recorded_events" ||
           key == "truncated" || key == "valid" || key == "complete" ||
           key == "errors" || key == "source_map" || key == "accesses" ||
           key == "events";
  }

  void put(Json value) {
    auto &frame = frames_.back();
    if (frame.value.is_array())
      frame.value.push_back(std::move(value));
    else
      frame.value[frame.key] = std::move(value);
  }

  bool scalar(Json value) {
    check_cancelled();
    if (!frames_.empty()) {
      put(std::move(value));
    } else if (depth_ == 0) {
      fail("trace root must be an object");
    } else if (depth_ == 1 && known(root_key_)) {
      if (root_key_ == "events" || root_key_ == "accesses")
        fail(root_key_ + " must be an array");
      header[root_key_] = std::move(value);
    } else if (depth_ == 2 && !array_.empty()) {
      fail(array_ + " entries must be objects");
    }
    return true;
  }

  void finish_frame() {
    auto value = std::move(frames_.back().value);
    frames_.pop_back();
    if (!frames_.empty())
      put(std::move(value));
    else if (depth_ == 2)
      header[root_key_] = std::move(value);
    else if (array_ == "events")
      on_event_(std::move(value));
    else
      on_access_(std::move(value));
  }

  std::function<bool()> cancelled_;
  Consumer on_access_;
  Consumer on_event_;
  std::vector<Frame> frames_;
  std::set<std::string> seen_;
  std::string root_key_;
  std::string array_;
  std::size_t depth_ = 0;
  bool root_seen_ = false;
};
} // namespace

static Json parse(std::ifstream &input, TraceSax::Consumer on_access,
                  TraceSax::Consumer on_event, U64 &count,
                  std::function<bool()> cancelled = {}) {
  TraceSax handler(
      std::move(on_access),
      [&](Json record) {
        const auto sequence =
            unsigned_value(field(record, "sequence"), "sequence");
        if (sequence != count)
          fail("sequence must start at zero and be consecutive; expected " +
               std::to_string(count));
        try {
          on_event(std::move(record));
        } catch (const std::ios_base::failure &) {
          throw;
        } catch (const std::bad_alloc &) {
          throw;
        } catch (const std::exception &error) {
          fail("event " + std::to_string(sequence) + ": " + error.what());
        }
        count = add(count, 1);
      },
      std::move(cancelled));
  if (!Json::sax_parse(input, &handler) || input.bad())
    fail("cannot read trace JSON");
  handler.validate_header(count);
  return handler.header;
}

static U64 end_offset(const Event &entry) {
  if (entry.offset >= 0)
    return add(static_cast<U64>(entry.offset), entry.size);
  const auto negative = magnitude(entry.offset);
  return entry.size > negative ? entry.size - negative : 0;
}

static void place_buffers(std::map<U64, BufferLayout> &buffers) {
  U64 cursor = 0x10000000;
  for (auto &[id, layout] : buffers) {
    (void)id;
    const auto negative =
        layout.min_offset < 0 ? magnitude(layout.min_offset) : 0;
    const auto candidate = add(cursor, negative);
    layout.base_address = add(candidate, (64 - candidate % 64) % 64);
    cursor = add(layout.base_address, layout.end_offset);
  }
}

static U64 address(const Event &entry, const BufferLayout &layout) {
  if (entry.offset < layout.min_offset || end_offset(entry) > layout.end_offset)
    fail("trace changed between passes: access exceeds buffer layout");
  return entry.offset < 0
             ? layout.base_address - magnitude(entry.offset)
             : add(layout.base_address, static_cast<U64>(entry.offset));
}

static void record(LineStatistics &stats,
                   const HierarchyLineAccessResult &line) {
  stats.probes = add(stats.probes, 1);
  if (line.hit)
    stats.hits = add(stats.hits, 1);
  else
    stats.misses = add(stats.misses, 1);
}

static void record(LevelStatistics &stats,
                   const HierarchyLineAccessResult &line, bool write) {
  record(stats.total, line);
  record(write ? stats.writes : stats.reads, line);
}

static void record_eviction(LevelStatistics &stats, const CacheLine &,
                            bool write) {
  for (auto *counter : {&stats.total, write ? &stats.writes : &stats.reads}) {
    counter->evictions = add(counter->evictions, 1);
  }
}

static std::string hex_address(U64 value) {
  std::ostringstream text;
  text << "0x" << std::hex << value;
  return text.str();
}

static Json json_totals(const LevelStatistics &stats) {
  Json result{{"read_line_accesses", stats.reads.probes},
              {"write_line_accesses", stats.writes.probes},
              {"hits", stats.total.hits},
              {"misses", stats.total.misses},
              {"read_hits", stats.reads.hits},
              {"read_misses", stats.reads.misses},
              {"write_hits", stats.writes.hits},
              {"write_misses", stats.writes.misses},
              {"evictions", stats.total.evictions},
              {"writebacks", stats.total.writebacks},
              {"back_invalidations", stats.total.back_invalidations}};
  const auto rate = stats.total.hit_rate_percent();
  result["hit_rate_percent"] = rate ? Json(*rate) : Json(nullptr);
  return result;
}

static Json json_processed(U64 events, const LevelStatistics &stats) {
  return {{"trace_events", events}, {"line_accesses", stats.total.probes}};
}

static Json json_config(const CacheConfig &config) {
  return {{"size_bytes", config.size_bytes},
          {"line_size_bytes", config.line_size_bytes},
          {"associativity", config.associativity},
          {"set_count",
           config.size_bytes / config.line_size_bytes / config.associativity},
          {"replacement_policy", config.replacement_policy},
          {"write_policy", to_string(config.write_policy)},
          {"allocation_policy", to_string(config.allocation_policy)}};
}

static Json json_levels(const std::vector<LevelStatistics> &levels,
                        const std::vector<U64> &events) {
  Json result = Json::array();
  for (std::size_t index = 0; index < levels.size(); ++index)
    result.push_back(
        {{"level", index + 1},
         {"processed", json_processed(events[index], levels[index])},
         {"totals", json_totals(levels[index])}});
  return result;
}

static Json replay_event(Json entry, RunSummary &summary,
                         CacheHierarchy &hierarchy) {
  const auto access = event(entry);
  const auto found = summary.instructions.find(access.access_id);
  if (found == summary.instructions.end() ||
      found->second.memref_id != access.access.memref_id ||
      found->second.kind != access.access.kind)
    fail("trace access changed between passes");
  const auto layout = summary.buffers.find(access.access.memref_id);
  if (layout == summary.buffers.end())
    fail("trace buffer changed between passes");
  const auto model_address = address(access, layout->second);
  const bool write = access.access.kind == "write";
  const auto result =
      write ? hierarchy.write(model_address,
                              static_cast<std::size_t>(access.size))
            : hierarchy.read(model_address,
                             static_cast<std::size_t>(access.size));
  std::vector<Json> line_results(summary.levels.size(), Json::array());
  auto &instruction = found->second;
  for (const auto &line : result.lines) {
    const auto index = line.level - 1;
    record(summary.levels.at(index), line, write);
    record(instruction.levels.at(index), line, write);
    line_results.at(index).push_back(
        {{"line_address", hex_address(line.line_address)},
         {"hit", line.hit},
         {"set", line.set},
         {"way", line.way ? Json(*line.way) : Json(nullptr)},
         {"evicted_address", line.evicted
                                 ? Json(hex_address(line.evicted->line_address))
                                 : Json(nullptr)},
         {"evicted_dirty",
          line.evicted ? Json(line.evicted->dirty) : Json(nullptr)}});
  }
  Json evictions = Json::array();
  for (const auto &eviction : result.evictions) {
    record_eviction(summary.levels.at(eviction.level - 1), eviction.line,
                    write);
    record_eviction(instruction.levels.at(eviction.level - 1), eviction.line,
                    write);
    evictions.push_back(
        {{"level", eviction.level},
         {"line_address", hex_address(eviction.line.line_address)},
         {"dirty", eviction.line.dirty}});
  }
  Json invalidations = Json::array(), writebacks = Json::array();
  for (const auto &item : result.back_invalidations) {
    for (auto *stats : {&summary.levels.at(item.level - 1),
                        &instruction.levels.at(item.level - 1)})
      for (auto *counter :
           {&stats->total, write ? &stats->writes : &stats->reads})
        counter->back_invalidations = add(counter->back_invalidations, 1);
    invalidations.push_back(
        {{"level", item.level},
         {"line_address", hex_address(item.line.line_address)},
         {"dirty", item.line.dirty}});
  }
  for (const auto &item : result.writebacks) {
    for (auto *stats : {&summary.levels.at(item.level - 1),
                        &instruction.levels.at(item.level - 1)})
      for (auto *counter :
           {&stats->total, write ? &stats->writes : &stats->reads})
        counter->writebacks = add(counter->writebacks, 1);
    writebacks.push_back({{"level", item.level},
                          {"line_address", hex_address(item.line.line_address)},
                          {"dirty", item.line.dirty}});
  }
  Json level_results = Json::array();
  const auto primary_lines = line_results.front();
  for (std::size_t index = 0; index < line_results.size(); ++index) {
    if (line_results[index].empty())
      continue;
    summary.level_events[index] = add(summary.level_events[index], 1);
    instruction.level_events[index] = add(instruction.level_events[index], 1);
    level_results.push_back(
        {{"level", index + 1}, {"lines", std::move(line_results[index])}});
  }
  summary.memory_reads = add(summary.memory_reads, result.memory_reads);
  summary.memory_writes = add(summary.memory_writes, result.memory_writes);
  summary.memory_write_bytes =
      add(summary.memory_write_bytes, result.memory_write_bytes);
  summary.memory_writebacks =
      add(summary.memory_writebacks, result.memory_writebacks);
  summary.events = add(summary.events, 1);
  auto &count = write ? summary.writes : summary.reads;
  auto &bytes = write ? summary.write_bytes : summary.read_bytes;
  count = add(count, 1);
  bytes = add(bytes, access.size);
  instruction.events = add(instruction.events, 1);
  instruction.requested_bytes = add(instruction.requested_bytes, access.size);
  return Json{{"sequence", access.sequence},
              {"access_id", access.access_id},
              {"kind", access.access.kind},
              {"address", hex_address(model_address)},
              {"size_bytes", access.size},
              {"lines", primary_lines},
              {"level_results", std::move(level_results)},
              {"evictions", std::move(evictions)},
              {"back_invalidations", std::move(invalidations)},
              {"writebacks", std::move(writebacks)},
              {"memory_writes", result.memory_writes},
              {"memory_write_bytes", result.memory_write_bytes},
              {"memory_reads", result.memory_reads},
              {"memory_writebacks", result.memory_writebacks},
              {"trace_event", std::move(entry)}};
}

namespace {
class ReportFile {
public:
  explicit ReportFile(fs::path destination)
      : destination_(std::move(destination)) {
    if (fs::exists(destination_) && !fs::is_regular_file(destination_))
      fail("report destination must be a regular file");
    auto parent = destination_.parent_path();
    if (parent.empty())
      parent = ".";
    fs::create_directories(parent);
    auto pattern =
        (parent / (destination_.filename().string() + ".tmp.XXXXXX")).string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int descriptor = mkstemp(name.data());
    if (descriptor == -1)
      fail("cannot create temporary report in " + parent.string());
    temporary_ = name.data();
    file_ = fdopen(descriptor, "wb");
    if (!file_) {
      close(descriptor);
      fs::remove(temporary_);
      fail("cannot open temporary report");
    }
  }

  ReportFile(const ReportFile &) = delete;
  ReportFile &operator=(const ReportFile &) = delete;

  ~ReportFile() {
    if (file_)
      std::fclose(file_);
    std::error_code ignored;
    if (!temporary_.empty())
      fs::remove(temporary_, ignored);
  }

  const fs::path &destination() const { return destination_; }
  bool published() const { return published_; }

  void write(const std::string &text) {
    if (std::fwrite(text.data(), 1, text.size(), file_) != text.size())
      fail("cannot write report");
  }

  void finish() {
    const auto flushed = std::fflush(file_);
    const auto closed = std::fclose(file_);
    file_ = nullptr;
    if (flushed != 0 || closed != 0)
      fail("cannot flush/close report");
  }

  void publish() {
    fs::rename(temporary_, destination_);
    temporary_.clear();
    published_ = true;
  }

private:
  fs::path destination_;
  fs::path temporary_;
  std::FILE *file_ = nullptr;
  bool published_ = false;
};

// Roll back ordinary publication failures for the two-file report. This is not
// a crash-atomic transaction; a bundle must not be read or written
// concurrently.
class ReportBackup {
public:
  explicit ReportBackup(const ReportFile &report) : report_(report) {
    if (!fs::exists(report.destination()))
      return;
    auto pattern = report.destination().string() + ".previous.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int descriptor = mkstemp(name.data());
    if (descriptor == -1)
      fail("cannot preserve previous report");
    close(descriptor);
    backup_ = name.data();
    try {
      fs::rename(report.destination(), backup_);
    } catch (...) {
      fs::remove(backup_);
      throw;
    }
  }

  ~ReportBackup() {
    std::error_code ignored;
    if (committed_) {
      if (!backup_.empty())
        fs::remove(backup_, ignored);
    } else if (!backup_.empty()) {
      fs::rename(backup_, report_.destination(), ignored);
    } else if (report_.published()) {
      fs::remove(report_.destination(), ignored);
    }
  }

  void commit() { committed_ = true; }

private:
  const ReportFile &report_;
  fs::path backup_;
  bool committed_ = false;
};
} // namespace

static void publish_reports(ReportFile &statistics, ReportFile &events) {
  statistics.finish();
  events.finish();
  ReportBackup old_statistics(statistics);
  ReportBackup old_events(events);
  events.publish();
  statistics.publish();
  old_events.commit();
  old_statistics.commit();
}

} // namespace runner_detail

std::optional<double> LineStatistics::hit_rate_percent() const {
  if (probes == 0)
    return std::nullopt;
  return 100.0 * (static_cast<double>(hits) / static_cast<double>(probes));
}

RunSummary run_trace(const std::filesystem::path &input_path,
                     CacheHierarchy &hierarchy,
                     const std::filesystem::path &report_path) {
  using namespace runner_detail;
  try {
    std::ifstream input(input_path, std::ios::binary);
    if (!input)
      fail("cannot open trace");
    const auto events_path = report_path.parent_path() / "events.json";
    if (fs::absolute(report_path).lexically_normal() ==
        fs::absolute(events_path).lexically_normal())
      fail("statistics path must differ from events.json");
    for (const auto &path : {report_path, events_path})
      if (fs::exists(path) && fs::equivalent(input_path, path))
        fail("report path must differ from trace path");
    if (fs::exists(report_path) && fs::exists(events_path) &&
        fs::equivalent(report_path, events_path))
      fail("statistics and events must be different files");
    const auto original_size = fs::file_size(input_path);
    const auto original_time = fs::last_write_time(input_path);
    const auto unchanged = [&] {
      if (fs::file_size(input_path) != original_size ||
          fs::last_write_time(input_path) != original_time)
        fail("trace changed between passes");
    };

    RunSummary summary;
    std::map<U64, Json> sites;
    std::map<U64, AccessSite> observed;
    U64 first_count = 0;
    const auto header = parse(
        input,
        [&](const Json &entry) {
          const auto id =
              unsigned_value(field(entry, "access_id"), "access_id");
          site(entry);
          string_field(entry, "source_location");
          if (!sites.emplace(id, entry).second)
            fail("duplicate access_id in accesses table");
        },
        [&](const Json &entry) {
          const auto access = event(entry);
          const auto inserted =
              observed.emplace(access.access_id, access.access);
          if (!inserted.second && !(inserted.first->second == access.access))
            fail("access_id changes memref_id/kind");
          auto &layout = summary.buffers[access.access.memref_id];
          layout.min_offset = std::min(layout.min_offset, access.offset);
          layout.end_offset = std::max(layout.end_offset, end_offset(access));
        },
        first_count);
    for (const auto &[id, access] : observed) {
      const auto found = sites.find(id);
      if (found == sites.end() || !(site(found->second) == access))
        fail("event access_id does not match accesses table: " +
             std::to_string(id));
    }
    place_buffers(summary.buffers);
    unchanged();

    summary.level_names = hierarchy.level_names();
    summary.levels.resize(summary.level_names.size());
    summary.level_events.resize(summary.level_names.size());
    for (const auto &[id, entry] : sites) {
      const auto access = site(entry);
      InstructionStatistics instruction;
      instruction.memref_id = access.memref_id;
      instruction.kind = access.kind;
      instruction.levels.resize(summary.levels.size());
      instruction.level_events.resize(summary.levels.size());
      summary.instructions.emplace(id, std::move(instruction));
    }

    ReportFile statistics(report_path);
    ReportFile events(events_path);
    Json names = Json::array();
    for (std::size_t index = 0; index < summary.level_names.size(); ++index)
      names.push_back(
          {{"level", index + 1},
           {"name", summary.level_names[index]},
           {"cache_config", json_config(hierarchy.configuration(index + 1))}});
    Json buffers = Json::array();
    for (const auto &[id, layout] : summary.buffers)
      buffers.push_back({{"memref_id", id},
                         {"base_address", hex_address(layout.base_address)},
                         {"min_offset", layout.min_offset},
                         {"end_offset", layout.end_offset}});
    events.write(
        "{\"schema\":\"cachescope.simulation-events.v1\",\"variant\":" +
        header.at("variant").dump() + ",\"levels\":" + names.dump() +
        ",\"events\":[");

    hierarchy.clear();
    input.clear();
    input.seekg(0);
    if (!input)
      fail("cannot rewind trace");
    U64 second_count = 0;
    const auto second_header = parse(
        input,
        [&](const Json &entry) {
          const auto id =
              unsigned_value(field(entry, "access_id"), "access_id");
          const auto found = sites.find(id);
          if (found == sites.end() || found->second != entry)
            fail("trace accesses table changed between passes");
        },
        [&](Json entry) {
          const auto result =
              replay_event(std::move(entry), summary, hierarchy);
          if (second_count != 0)
            events.write(",");
          events.write(result.dump());
        },
        second_count);
    if (header != second_header || first_count != second_count)
      fail("trace header changed between passes");
    unchanged();
    events.write("]}\n");

    auto levels = json_levels(summary.levels, summary.level_events);
    for (std::size_t index = 0; index < levels.size(); ++index) {
      levels[index]["name"] = summary.level_names[index];
      levels[index]["cache_config"] =
          json_config(hierarchy.configuration(index + 1));
    }
    Json report{
        {"schema", "cachescope.simulation-statistics.v1"},
        {"variant", header.at("variant")},
        {"producer", header.at("producer").at("name")},
        {"cache_config", json_config(hierarchy.configuration(1))},
        {"processed", json_processed(summary.events, summary.levels.front())},
        {"totals", json_totals(summary.levels.front())},
        {"levels", std::move(levels)},
        {"buffers", std::move(buffers)},
        {"source", header},
        {"hierarchy", describe_policies(hierarchy)},
        {"requests",
         {{"events", summary.events},
          {"reads", summary.reads},
          {"writes", summary.writes},
          {"read_bytes", summary.read_bytes},
          {"write_bytes", summary.write_bytes}}},
        {"memory",
         {{"reads", summary.memory_reads},
          {"writes", summary.memory_writes},
          {"write_bytes", summary.memory_write_bytes},
          {"writebacks", summary.memory_writebacks}}}};
    auto prefix = report.dump();
    prefix.pop_back();
    statistics.write(prefix + ",\"instructions\":[");
    bool first_instruction = true;
    for (const auto &[id, instruction] : summary.instructions) {
      if (!first_instruction)
        statistics.write(",");
      first_instruction = false;
      auto record = sites.at(id);
      record["events"] = instruction.events;
      record["requested_bytes"] = instruction.requested_bytes;
      record["levels"] =
          json_levels(instruction.levels, instruction.level_events);
      statistics.write(record.dump());
    }
    statistics.write("]}\n");
    publish_reports(statistics, events);
    return summary;
  } catch (const std::exception &error) {
    fail("trace " + input_path.string() + ": " + error.what());
  }
}

namespace {
struct TraceStamp {
  struct stat value{};

  explicit TraceStamp(const std::filesystem::path &path) {
    if (::stat(path.c_str(), &value) != 0 || !S_ISREG(value.st_mode))
      throw TraceChanged("trace is missing or is not a regular file");
  }

  bool operator==(const TraceStamp &other) const {
#ifdef __APPLE__
    const auto mtime = value.st_mtimespec;
    const auto ctime = value.st_ctimespec;
    const auto other_mtime = other.value.st_mtimespec;
    const auto other_ctime = other.value.st_ctimespec;
#else
    const auto mtime = value.st_mtim;
    const auto ctime = value.st_ctim;
    const auto other_mtime = other.value.st_mtim;
    const auto other_ctime = other.value.st_ctim;
#endif
    return value.st_dev == other.value.st_dev &&
           value.st_ino == other.value.st_ino &&
           value.st_size == other.value.st_size &&
           mtime.tv_sec == other_mtime.tv_sec &&
           mtime.tv_nsec == other_mtime.tv_nsec &&
           ctime.tv_sec == other_ctime.tv_sec &&
           ctime.tv_nsec == other_ctime.tv_nsec;
  }
};
} // namespace

struct TraceReplay::Impl {
  using Json = runner_detail::Json;
  std::filesystem::path path;
  CacheHierarchy &hierarchy;
  TraceStamp stamp;
  std::unique_ptr<std::FILE, int (*)(std::FILE *)> spool{nullptr, &std::fclose};
  RunSummary current;
  RunSummary cold;
  Json header;
  std::map<std::uint64_t, Json> sites;
  std::uint64_t total = 0;
  std::uint64_t position = 0;
  bool valid = true;

  Impl(const std::filesystem::path &trace, CacheHierarchy &cache,
       const std::function<bool()> &cancelled)
      : path(trace), hierarchy(cache), stamp(trace) {
    using namespace runner_detail;
    std::ifstream input(trace, std::ios::binary);
    if (!input)
      fail("cannot open trace");
    spool.reset(std::tmpfile());
    if (!spool)
      throw std::ios_base::failure("cannot create temporary replay spool");
    std::map<U64, AccessSite> observed;
    header = parse(
        input,
        [&](const Json &entry) {
          const auto id =
              unsigned_value(field(entry, "access_id"), "access_id");
          site(entry);
          string_field(entry, "source_location");
          if (!sites.emplace(id, entry).second)
            fail("duplicate access_id in accesses table");
        },
        [&](Json entry) {
          const auto access = event(entry);
          const auto inserted =
              observed.emplace(access.access_id, access.access);
          if (!inserted.second && !(inserted.first->second == access.access))
            fail("access_id changes memref_id/kind");
          auto &layout = current.buffers[access.access.memref_id];
          layout.min_offset = std::min(layout.min_offset, access.offset);
          layout.end_offset = std::max(layout.end_offset, end_offset(access));
          Json normalized;
          for (const auto *key :
               {"sequence", "access_id", "memref_id", "byte_offset",
                "size_bytes", "kind", "address_kind"})
            normalized[key] = entry.at(key);
          const auto line = normalized.dump() + '\n';
          if (std::fwrite(line.data(), 1, line.size(), spool.get()) !=
              line.size())
            throw std::ios_base::failure("cannot write replay spool");
        },
        total, cancelled);
    for (const auto &[id, access] : observed) {
      const auto found = sites.find(id);
      if (found == sites.end() || !(site(found->second) == access))
        fail("event does not match static access table");
    }
    place_buffers(current.buffers);
    current.level_names = hierarchy.level_names();
    current.levels.resize(current.level_names.size());
    current.level_events.resize(current.level_names.size());
    for (const auto &[id, entry] : sites) {
      const auto access = site(entry);
      InstructionStatistics instruction;
      instruction.memref_id = access.memref_id;
      instruction.kind = access.kind;
      instruction.levels.resize(current.levels.size());
      instruction.level_events.resize(current.levels.size());
      current.instructions.emplace(id, std::move(instruction));
    }
    if (std::fflush(spool.get()) != 0)
      throw std::ios_base::failure("cannot flush replay spool");
    rewind();
    for (U64 index = 0; index < total; ++index) {
      if (cancelled && cancelled())
        fail("preparation cancelled");
      const auto access = event(read());
      const auto begin =
          address(access, current.buffers.at(access.access.memref_id));
      add(begin, access.size - 1);
    }
    unchanged();
    cold = current;
    rewind();
    hierarchy.clear();
  }

  void unchanged() const {
    if (!(stamp == TraceStamp(path)))
      throw TraceChanged("trace changed since preparation");
  }

  void rewind() {
    if (std::fseek(spool.get(), 0, SEEK_SET) != 0)
      throw std::ios_base::failure("cannot rewind replay spool");
    std::clearerr(spool.get());
  }

  Json read() {
    std::string line;
    for (;;) {
      const int c = std::fgetc(spool.get());
      if (c == EOF)
        throw std::ios_base::failure("cannot read replay spool");
      if (c == '\n')
        break;
      line.push_back(static_cast<char>(c));
    }
    return Json::parse(line);
  }
};

TraceReplay::TraceReplay(const std::filesystem::path &trace,
                         CacheHierarchy &hierarchy,
                         const std::function<bool()> &cancelled)
    : impl_(std::make_unique<Impl>(trace, hierarchy, cancelled)) {}
TraceReplay::~TraceReplay() = default;
std::uint64_t TraceReplay::total_events() const { return impl_->total; }
std::uint64_t TraceReplay::cursor() const { return impl_->position; }
const RunSummary &TraceReplay::summary() const {
  if (!impl_->valid)
    throw std::runtime_error("failed replay requires reset");
  return impl_->current;
}
void TraceReplay::check_unchanged() const { impl_->unchanged(); }

nlohmann::json TraceReplay::step() {
  if (!impl_->valid)
    throw std::runtime_error("failed replay requires reset");
  try {
    impl_->unchanged();
    if (impl_->position == impl_->total)
      return nullptr;
    auto result = runner_detail::replay_event(impl_->read(), impl_->current,
                                              impl_->hierarchy);
    impl_->position = runner_detail::add(impl_->position, 1);
    return result;
  } catch (...) {
    impl_->valid = false;
    throw;
  }
}

void TraceReplay::reset() {
  impl_->valid = false;
  impl_->unchanged();
  impl_->hierarchy.clear();
  impl_->rewind();
  impl_->current = impl_->cold;
  impl_->position = 0;
  impl_->valid = true;
}

nlohmann::json TraceReplay::metadata() const {
  using namespace runner_detail;
  Json buffers = Json::array(), accesses = Json::array();
  for (const auto &[id, layout] : impl_->current.buffers)
    buffers.push_back({{"memref_id", id},
                       {"base_address", hex_address(layout.base_address)},
                       {"min_offset", layout.min_offset},
                       {"end_offset", layout.end_offset}});
  for (const auto &[id, record] : impl_->sites) {
    (void)id;
    accesses.push_back(record);
  }
  return {{"source", impl_->header},
          {"buffers", std::move(buffers)},
          {"accesses", std::move(accesses)}};
}

nlohmann::json TraceReplay::instructions(std::uint64_t offset,
                                         std::size_t limit) const {
  using namespace runner_detail;
  Json result = Json::array();
  const auto &instructions = summary().instructions;
  auto iterator = instructions.begin();
  if (offset > instructions.size())
    fail("instruction offset out of range");
  std::advance(iterator, static_cast<std::ptrdiff_t>(offset));
  for (; iterator != instructions.end() && result.size() < limit; ++iterator) {
    const auto &[id, instruction] = *iterator;
    auto row = impl_->sites.at(id);
    row["events"] = instruction.events;
    row["requested_bytes"] = instruction.requested_bytes;
    row["levels"] = json_levels(instruction.levels, instruction.level_events);
    result.push_back(std::move(row));
  }
  return result;
}

nlohmann::json TraceReplay::statistics() const {
  using namespace runner_detail;
  const auto &s = summary();
  return {{"processed", json_processed(s.events, s.levels.front())},
          {"totals", json_totals(s.levels.front())},
          {"levels", json_levels(s.levels, s.level_events)},
          {"requests",
           {{"events", s.events},
            {"reads", s.reads},
            {"writes", s.writes},
            {"read_bytes", s.read_bytes},
            {"write_bytes", s.write_bytes}}},
          {"memory",
           {{"reads", s.memory_reads},
            {"writes", s.memory_writes},
            {"write_bytes", s.memory_write_bytes},
            {"writebacks", s.memory_writebacks}}}};
}

} // namespace cache_simulator
