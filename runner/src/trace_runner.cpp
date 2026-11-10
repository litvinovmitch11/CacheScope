#include "cache_simulator/trace_runner.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
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

static const Json &field(const Json &object, const std::string &name) {
  if (!object.contains(name))
    fail("missing field: " + name);
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
  return {unsigned_value(field(record, "memref_id"), "memref_id"),
          access_kind(record)};
}

static Event event(const Json &record) {
  if (string_field(record, "address_kind") != "memref_offset")
    fail("unsupported address_kind; expected memref_offset");
  Event result{unsigned_value(field(record, "sequence"), "sequence"),
               unsigned_value(field(record, "access_id"), "access_id"),
               site(record),
               signed_value(field(record, "byte_offset"), "byte_offset"),
               unsigned_value(field(record, "size_bytes"), "size_bytes")};
  if (result.size == 0 || result.size > std::numeric_limits<std::size_t>::max())
    fail("size_bytes must be positive and fit size_t");
  const auto &indices = field(record, "indices");
  if (!indices.is_array())
    fail("indices must be an array");
  for (const auto &index : indices)
    signed_value(index, "index");
  return result;
}

// Only the current accesses[]/events[] record is materialized. Unknown root
// fields are skipped without accumulating a document-sized DOM.
namespace {
class TraceSax : public nlohmann::json_sax<Json> {
public:
  using Consumer = std::function<void(Json)>;

  TraceSax(Consumer on_access, Consumer on_event)
      : on_access_(std::move(on_access)), on_event_(std::move(on_event)) {}

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
    if (depth_ == 0) {
      if (root_seen_)
        fail("expected one root object");
      root_seen_ = true;
    } else if (!frames_.empty() || (depth_ == 2 && !array_.empty())) {
      frames_.push_back({Json::object(), {}});
    } else if (depth_ == 1 && known(root_key_)) {
      fail("invalid object value for " + root_key_);
    }
    ++depth_;
    return true;
  }

  bool end_object() override {
    if (!frames_.empty())
      finish_frame();
    --depth_;
    return true;
  }

  bool start_array(std::size_t) override {
    if (depth_ == 0)
      fail("trace root must be an object");
    if (!frames_.empty()) {
      frames_.push_back({Json::array(), {}});
    } else if (depth_ == 1 && known(root_key_)) {
      if (root_key_ != "events" && root_key_ != "accesses")
        fail("invalid array value for " + root_key_);
      array_ = root_key_;
    } else if (depth_ == 2 && !array_.empty()) {
      fail(array_ + " entries must be objects");
    }
    ++depth_;
    return true;
  }

  bool end_array() override {
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
         {"schema", "variant", "producer", "total_events", "recorded_events",
          "truncated", "accesses", "events"})
      if (!seen_.count(name))
        fail(std::string("missing root field: ") + name);
    if (string_field(header, "schema") != "cachescope.trace.v1")
      fail("unsupported trace schema");
    string_field(header, "variant");
    string_field(header, "producer");
    if (!field(header, "truncated").is_boolean())
      fail("truncated must be boolean");
    if (header.at("truncated").get<bool>())
      fail("truncated traces are not supported");
    const auto recorded =
        unsigned_value(field(header, "recorded_events"), "recorded_events");
    const auto total =
        unsigned_value(field(header, "total_events"), "total_events");
    if (recorded != count || total != count)
      fail("event counts do not match the complete trace");
  }

private:
  struct Frame {
    Json value;
    std::string key;
  };

  static bool known(const std::string &key) {
    return key == "schema" || key == "variant" || key == "producer" ||
           key == "total_events" || key == "recorded_events" ||
           key == "truncated" || key == "accesses" || key == "events";
  }

  void put(Json value) {
    auto &frame = frames_.back();
    if (frame.value.is_array())
      frame.value.push_back(std::move(value));
    else
      frame.value[frame.key] = std::move(value);
  }

  bool scalar(Json value) {
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
    else if (array_ == "events")
      on_event_(std::move(value));
    else
      on_access_(std::move(value));
  }

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
                  TraceSax::Consumer on_event, U64 &count) {
  TraceSax handler(std::move(on_access), [&](Json record) {
    const auto sequence = unsigned_value(field(record, "sequence"), "sequence");
    if (sequence != count)
      fail("sequence must start at zero and be consecutive; expected " +
           std::to_string(count));
    try {
      on_event(std::move(record));
    } catch (const std::exception &error) {
      fail("event " + std::to_string(sequence) + ": " + error.what());
    }
    count = add(count, 1);
  });
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

static void record(LineStatistics &stats, const LineAccessResult &line) {
  stats.probes = add(stats.probes, 1);
  if (line.hit)
    stats.hits = add(stats.hits, 1);
  else
    stats.misses = add(stats.misses, 1);
  if (line.evicted_address)
    stats.evictions = add(stats.evictions, 1);
}

static void record(LevelStatistics &stats, const LineAccessResult &line,
                   bool write) {
  record(stats.total, line);
  record(write ? stats.writes : stats.reads, line);
}

static Json json_stats(const LineStatistics &stats) {
  Json result{{"probes", stats.probes},
              {"hits", stats.hits},
              {"misses", stats.misses},
              {"evictions", stats.evictions}};
  const auto rate = stats.hit_rate_percent();
  result["hit_rate_percent"] = rate ? Json(*rate) : Json(nullptr);
  return result;
}

static Json json_levels(const std::vector<LevelStatistics> &levels) {
  Json result = Json::array();
  for (std::size_t index = 0; index < levels.size(); ++index)
    result.push_back({{"level", index + 1},
                      {"total", json_stats(levels[index].total)},
                      {"reads", json_stats(levels[index].reads)},
                      {"writes", json_stats(levels[index].writes)}});
  return result;
}

namespace {
class ReportFile {
public:
  explicit ReportFile(const fs::path &destination) : destination_(destination) {
    auto parent = destination.parent_path();
    if (parent.empty())
      parent = ".";
    fs::create_directories(parent);
    auto pattern =
        (parent / (destination.filename().string() + ".tmp.XXXXXX")).string();
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
    if (!temporary_.empty()) {
      std::error_code ignored;
      fs::remove(temporary_, ignored);
    }
  }

  void write(const std::string &text) {
    if (std::fwrite(text.data(), 1, text.size(), file_) != text.size())
      fail("cannot write report");
  }

  void publish() {
    const auto flush = std::fflush(file_);
    const auto closed = std::fclose(file_);
    file_ = nullptr;
    if (flush != 0 || closed != 0)
      fail("cannot flush/close report");
    fs::rename(temporary_, destination_);
    temporary_.clear();
  }

private:
  fs::path destination_;
  fs::path temporary_;
  std::FILE *file_ = nullptr;
};
} // namespace

} // namespace runner_detail

std::optional<double> LineStatistics::hit_rate_percent() const {
  if (probes == 0)
    return std::nullopt;
  return 100.0 * (static_cast<double>(hits) / static_cast<double>(probes));
}

RunSummary run_trace(const std::filesystem::path &input_path,
                     ICacheSimulator &cache,
                     const std::filesystem::path &report_path) {
  using namespace runner_detail;
  try {
    std::ifstream input(input_path, std::ios::binary);
    if (!input)
      fail("cannot open trace");
    if (fs::exists(report_path) && fs::equivalent(input_path, report_path))
      fail("report path must differ from trace path");
    const auto original_size = fs::file_size(input_path);
    const auto original_time = fs::last_write_time(input_path);
    const auto unchanged = [&] {
      if (fs::file_size(input_path) != original_size ||
          fs::last_write_time(input_path) != original_time)
        fail("trace changed between passes");
    };

    RunSummary summary;
    std::map<U64, AccessSite> sites;
    std::map<U64, AccessSite> observed;
    U64 first_count = 0;
    const auto header = parse(
        input,
        [&](const Json &entry) {
          const auto id =
              unsigned_value(field(entry, "access_id"), "access_id");
          if (!sites.emplace(id, site(entry)).second)
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
      if (found == sites.end() || !(found->second == access))
        fail("event access_id does not match accesses table: " +
             std::to_string(id));
    }
    place_buffers(summary.buffers);
    unchanged();

    summary.level_names = cache.level_names();
    if (summary.level_names.empty())
      fail("model must expose nonempty level names");
    for (const auto &name : summary.level_names)
      if (name.empty())
        fail("model must expose nonempty level names");
    summary.levels.resize(summary.level_names.size());
    for (const auto &[id, access] : sites)
      summary.instructions.emplace(
          id, InstructionStatistics{
                  access.memref_id, access.kind, 0, 0,
                  std::vector<LevelStatistics>(summary.levels.size())});

    ReportFile report(report_path);
    Json names = Json::array();
    for (std::size_t index = 0; index < summary.level_names.size(); ++index)
      names.push_back(
          {{"level", index + 1}, {"name", summary.level_names[index]}});
    Json buffers = Json::array();
    for (const auto &[id, layout] : summary.buffers)
      buffers.push_back({{"memref_id", id},
                         {"base_address", layout.base_address},
                         {"min_offset", layout.min_offset},
                         {"end_offset", layout.end_offset}});
    report.write("{\"schema\":\"cachescope.statistics.v1\",\"source\":" +
                 header.dump() + ",\"levels\":" + names.dump() +
                 ",\"buffers\":" + buffers.dump() + ",\"events\":[");

    cache.clear();
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
          if (found == sites.end() || !(found->second == site(entry)))
            fail("trace accesses table changed between passes");
        },
        [&](Json entry) {
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
              write ? cache.write(model_address,
                                  static_cast<std::size_t>(access.size))
                    : cache.read(model_address,
                                 static_cast<std::size_t>(access.size));
          if (result.lines.empty())
            fail("model returned no demand probes");
          bool visited_first = false;
          Json lines = Json::array();
          auto &instruction = found->second;
          for (const auto &line : result.lines) {
            if (line.level == 0 || line.level > summary.levels.size())
              fail("model returned invalid level: " +
                   std::to_string(line.level));
            visited_first = visited_first || line.level == 1;
            record(summary.levels[line.level - 1], line, write);
            record(instruction.levels[line.level - 1], line, write);
            lines.push_back({{"level", line.level},
                             {"line_address", line.line_address},
                             {"hit", line.hit},
                             {"set", line.set},
                             {"way", line.way},
                             {"evicted_address",
                              line.evicted_address ? Json(*line.evicted_address)
                                                   : Json(nullptr)}});
          }
          if (!visited_first)
            fail("model omitted first-level demand probes");
          summary.events = add(summary.events, 1);
          auto &count = write ? summary.writes : summary.reads;
          auto &bytes = write ? summary.write_bytes : summary.read_bytes;
          count = add(count, 1);
          bytes = add(bytes, access.size);
          instruction.events = add(instruction.events, 1);
          instruction.requested_bytes =
              add(instruction.requested_bytes, access.size);
          if (second_count != 0)
            report.write(",");
          report.write(Json{{"event", std::move(entry)},
                            {"model_address", model_address},
                            {"lines", std::move(lines)}}
                           .dump());
        },
        second_count);
    if (header != second_header || first_count != second_count)
      fail("trace header changed between passes");
    unchanged();
    report.write("],\"summary\":" +
                 Json{{"events", summary.events},
                      {"reads", summary.reads},
                      {"writes", summary.writes},
                      {"read_bytes", summary.read_bytes},
                      {"write_bytes", summary.write_bytes},
                      {"levels", json_levels(summary.levels)}}
                     .dump() +
                 ",\"instructions\":[");
    bool first_instruction = true;
    for (const auto &[id, instruction] : summary.instructions) {
      if (!first_instruction)
        report.write(",");
      first_instruction = false;
      report.write(Json{{"access_id", id},
                        {"memref_id", instruction.memref_id},
                        {"kind", instruction.kind},
                        {"events", instruction.events},
                        {"requested_bytes", instruction.requested_bytes},
                        {"levels", json_levels(instruction.levels)}}
                       .dump());
    }
    report.write("]}\n");
    report.publish();
    return summary;
  } catch (const std::exception &error) {
    fail("trace " + input_path.string() + ": " + error.what());
  }
}

} // namespace cache_simulator
