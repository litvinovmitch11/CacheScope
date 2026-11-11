#include "runner_api.hpp"
#include "cache_simulator/FIFO.h"
#include "cache_simulator/LRU.h"
#include "cache_simulator/Random.h"
#include "cache_simulator/SetAssociativeCacheLevel.h"
#include "cache_simulator/trace_replay.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace cache_simulator {
namespace http_detail {
using Json = nlohmann::json;
using U64 = std::uint64_t;
constexpr U64 safe_integer = 9007199254740991ULL;
constexpr U64 max_index =
    std::min<U64>(safe_integer, std::numeric_limits<std::size_t>::max());
const Json policies{{"inclusion_policy", "exclusive"},
                    {"write_policy", "write_back"},
                    {"allocation_policy", "write_allocate"}};
const Json limits{{"max_step_count", 1000},
                  {"max_sets_per_read", 64},
                  {"max_instructions_per_read", 100}};

namespace {
struct ApiError : std::runtime_error {
  int status;
  Json info;
  Json context = Json::object();
  ApiError(int http, std::string code, std::string message)
      : std::runtime_error(message), status(http),
        info{{"code", std::move(code)},
             {"message", std::move(message)},
             {"details", Json::object()}} {}
};
} // namespace

[[noreturn]] static void bad(const std::string &code,
                             const std::string &message) {
  throw ApiError(400, code, message);
}

static U64 decimal(const std::string &value, const std::string &code) {
  U64 result = 0;
  if (value.empty() || (value.size() > 1 && value.front() == '0') ||
      !std::all_of(value.begin(), value.end(),
                   [](char c) { return c >= '0' && c <= '9'; }))
    bad(code, "expected a canonical uint64 decimal string");
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
    bad(code, "uint64 value is out of range");
  return result;
}

static U64 decimal(const Json &value, const std::string &code) {
  if (!value.is_string())
    bad(code, "expected a decimal string");
  return decimal(value.get<std::string>(), code);
}

static std::size_t number(const Json &value, const std::string &code,
                          bool positive = true) {
  if (!value.is_number_integer() ||
      (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
    bad(code, "expected a nonnegative integer");
  const auto result = value.get<U64>();
  if (result > max_index || (positive && result == 0))
    bad(code, "integer is outside the supported range");
  return static_cast<std::size_t>(result);
}

static void keys(const Json &value,
                 std::initializer_list<const char *> required,
                 std::initializer_list<const char *> optional = {}) {
  if (!value.is_object())
    bad("invalid_request", "expected a JSON object");
  std::set<std::string> allowed;
  for (const auto *key : required) {
    if (!value.contains(key))
      bad("invalid_request", std::string("missing field: ") + key);
    allowed.insert(key);
  }
  allowed.insert(optional.begin(), optional.end());
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator)
    if (!allowed.count(iterator.key()))
      bad("invalid_request", "unknown field: " + iterator.key());
}

static std::string string(const Json &object, const char *key) {
  if (!object.contains(key) || !object.at(key).is_string())
    bad("invalid_request", std::string("expected string: ") + key);
  return object.at(key).get<std::string>();
}

static std::string hex(U64 address) {
  std::ostringstream out;
  out << "0x" << std::hex << address;
  return out.str();
}

static void counts(Json &object) {
  for (auto &value : object)
    value = value.dump();
}

static void level_statistics(Json &row) {
  counts(row.at("processed"));
  for (auto iterator = row.at("totals").begin();
       iterator != row.at("totals").end(); ++iterator)
    if (iterator.key() != "hit_rate_percent")
      iterator.value() = iterator.value().dump();
}

static Json summary_dto(TraceReplay &replay) {
  auto result = replay.statistics();
  level_statistics(result);
  for (auto &level : result.at("levels"))
    level_statistics(level);
  counts(result.at("requests"));
  counts(result.at("memory"));
  return result;
}

static Json outcome_dto(Json event) {
  Json trace;
  for (const auto *key :
       {"sequence", "access_id", "memref_id", "byte_offset", "size_bytes"})
    trace[key] = event.at("trace_event").at(key).dump();
  trace["kind"] = event.at("trace_event").at("kind");
  trace["address_kind"] = "memref_offset";
  return {{"trace_event", std::move(trace)},
          {"address", event.at("address")},
          {"level_results", event.at("level_results")},
          {"evictions", event.at("evictions")},
          {"memory_reads", event.at("memory_reads").dump()},
          {"memory_writebacks", event.at("memory_writebacks").dump()}};
}

static U64 metadata_id(const Json &value) {
  if (!value.is_number_integer() ||
      (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
    throw std::runtime_error("metadata ID must be uint64");
  return value.get<U64>();
}

static void source_location(const Json &location,
                            const std::set<std::string> &stages) {
  if (!location.is_object() ||
      !stages.count(location.value("stage_id", std::string{})) ||
      number(location.at("line"), "invalid_trace") == 0 ||
      !location.at("operation").is_string() ||
      location.at("operation").get<std::string>().empty())
    throw std::runtime_error("invalid source-map location");
  number(location.at("column"), "invalid_trace", false);
}

static Json access_dto(const Json &source) {
  Json result;
  for (const auto *key : {"access_id", "memref_id"})
    result[key] = std::to_string(metadata_id(source.at(key)));
  for (const auto *key : {"kind", "address_kind", "source_location"})
    result[key] = source.at(key);
  if (source.contains("origin_ids")) {
    if (!source.at("origin_ids").is_array())
      throw std::runtime_error("origin_ids must be an array");
    result["origin_ids"] = Json::array();
    for (const auto &id : source.at("origin_ids"))
      result["origin_ids"].push_back(std::to_string(metadata_id(id)));
  }
  if (source.contains("stage_locations"))
    result["stage_locations"] = source.at("stage_locations");
  return result;
}

static Json metadata_dto(TraceReplay &replay) {
  auto result = replay.metadata();
  auto &source = result.at("source");
  source["source_schema"] = source.at("schema");
  source.erase("schema");
  for (const auto *key : {"recorded_events", "total_events"})
    source[key] = source.at(key).dump();
  std::set<std::string> stages;
  std::set<std::string> stage_paths;
  std::set<U64> origins;
  if (source.contains("source_map")) {
    auto &mapping = source.at("source_map");
    if (!mapping.at("payload_fingerprint").is_string() ||
        !std::regex_match(mapping.at("payload_fingerprint").get<std::string>(),
                          std::regex("sha256:[0-9a-f]{64}")) ||
        !mapping.at("stages").is_array() || mapping.at("stages").empty() ||
        !mapping.at("origins").is_array())
      throw std::runtime_error("invalid source map");
    if (mapping.at("stages").front().at("stage_id") != "input")
      throw std::runtime_error("source_map must begin with the input stage");
    for (const auto &stage : mapping.at("stages")) {
      const auto id = stage.at("stage_id").get<std::string>();
      const auto path = stage.at("path").get<std::string>();
      const auto artifact = std::filesystem::path(path);
      if (!std::regex_match(id, std::regex("[a-z][a-z0-9-]*")) ||
          !stages.insert(id).second || path.empty() || artifact.is_absolute() ||
          path.find('\\') != std::string::npos ||
          path.find("//") != std::string::npos || path.back() == '/' ||
          std::any_of(
              artifact.begin(), artifact.end(),
              [](const auto &part) { return part == "." || part == ".."; }) ||
          !stage_paths.insert(path).second)
        throw std::runtime_error("invalid or duplicate source-map stage");
    }
    for (auto &origin : mapping.at("origins")) {
      source_location(origin, stages);
      if (origin.at("stage_id") != "input")
        throw std::runtime_error("origin must reference the input stage");
      const auto id = metadata_id(origin.at("origin_id"));
      if (!origins.insert(id).second)
        throw std::runtime_error("duplicate origin_id");
      origin["origin_id"] = std::to_string(id);
    }
    mapping["source_schema"] = mapping.at("schema");
    mapping.erase("schema");
  }
  for (auto &access : result.at("accesses")) {
    if (source.contains("source_map")) {
      if (!access.at("origin_ids").is_array() ||
          !access.at("stage_locations").is_array() ||
          access.at("origin_ids").empty() ||
          access.at("stage_locations").empty())
        throw std::runtime_error("missing source-map relationships");
      std::set<U64> seen;
      for (const auto &id : access.at("origin_ids"))
        if (!origins.count(metadata_id(id)) ||
            !seen.insert(metadata_id(id)).second)
          throw std::runtime_error("unknown or duplicate origin_id");
      for (const auto &location : access.at("stage_locations"))
        source_location(location, stages);
    } else if (access.contains("origin_ids") ||
               access.contains("stage_locations")) {
      throw std::runtime_error("source relationships require source_map");
    }
    access = access_dto(access);
  }
  for (auto &buffer : result.at("buffers"))
    for (const auto *key : {"memref_id", "min_offset", "end_offset"})
      buffer[key] = buffer.at(key).dump();
  return result;
}

// Observe actual level operations, including internal migrations, rather than
// reconstructing occupancy from the incomplete demand-outcome journal.
namespace {
class ObservedLevel final : public ICacheLevel {
public:
  ObservedLevel(const CacheConfig &config, std::unique_ptr<ICacheLevel> cache)
      : cache_(std::move(cache)),
        cells_(config.size_bytes / config.line_size_bytes /
                   config.associativity,
               std::vector<std::optional<CacheLine>>(config.associativity)) {
    if (!cache_)
      throw std::invalid_argument("cache factory returned null");
  }
  LookupResult lookup(U64 address) const override {
    return cache_->lookup(address);
  }
  void touch(U64 address, AccessType kind) override {
    const auto slot = cache_->lookup(address);
    cache_->touch(address, kind);
    if (kind == AccessType::Write)
      cells_.at(slot.set).at(slot.way.value())->dirty = true;
  }
  std::optional<CacheLine> invalidate(U64 address) override {
    const auto slot = cache_->lookup(address);
    auto line = cache_->invalidate(address);
    if (line)
      cells_.at(slot.set).at(slot.way.value()).reset();
    return line;
  }
  InsertResult insert(CacheLine line) override {
    auto result = cache_->insert(line);
    cells_.at(result.set).at(result.way) = line;
    return result;
  }
  void clear() override {
    cache_->clear();
    for (auto &set : cells_)
      for (auto &cell : set)
        cell.reset();
  }
  Json page(std::size_t offset, std::size_t limit) const {
    Json result = Json::array();
    const auto end = offset + std::min(limit, cells_.size() - offset);
    for (auto set = offset; set < end; ++set) {
      Json ways = Json::array();
      for (std::size_t way = 0; way < cells_[set].size(); ++way) {
        const auto &line = cells_[set][way];
        ways.push_back({{"way", way},
                        {"valid", bool(line)},
                        {"line_address",
                         line ? Json(hex(line->line_address)) : Json(nullptr)},
                        {"dirty", line ? line->dirty : false}});
      }
      result.push_back({{"set", set}, {"ways", std::move(ways)}});
    }
    return result;
  }

private:
  std::unique_ptr<ICacheLevel> cache_;
  std::vector<std::vector<std::optional<CacheLine>>> cells_;
};
} // namespace

static Json configuration(const Json &body) {
  keys(body, {"trace_path", "levels"});
  const auto path = string(body, "trace_path");
  if (path.size() < 2 || path.find('\0') != std::string::npos ||
      !std::filesystem::path(path).is_absolute())
    bad("invalid_request", "trace_path must be absolute");
  if (!body.at("levels").is_array() || body.at("levels").empty())
    bad("invalid_config", "levels must be a nonempty array");
  Json result = Json::array();
  std::set<std::string> names;
  std::size_t line_size = 0;
  for (const auto &entry : body.at("levels")) {
    keys(entry,
         {"name", "size_bytes", "line_size_bytes", "associativity",
          "replacement_policy"},
         {"random_seed"});
    const auto name = string(entry, "name");
    const auto policy = string(entry, "replacement_policy");
    const auto capacity = number(entry.at("size_bytes"), "invalid_config");
    const auto line = number(entry.at("line_size_bytes"), "invalid_config");
    const auto ways = number(entry.at("associativity"), "invalid_config");
    const auto sets = capacity / line / ways;
    if (name.empty() || !names.insert(name).second || (line & (line - 1)) ||
        !sets || (sets & (sets - 1)) || capacity % line ||
        (capacity / line) % ways || (line_size && line_size != line) ||
        (policy != "LRU" && policy != "FIFO" && policy != "Random"))
      bad("invalid_config", "invalid cache geometry, name or policy");
    line_size = line;
    Json seed = nullptr;
    if (policy == "Random")
      seed = std::to_string(
          entry.contains("random_seed")
              ? decimal(entry.at("random_seed"), "invalid_config")
              : 0);
    else if (entry.contains("random_seed"))
      bad("invalid_config", "random_seed is only allowed for Random");
    result.push_back({{"level", result.size() + 1},
                      {"name", name},
                      {"size_bytes", capacity},
                      {"line_size_bytes", line},
                      {"associativity", ways},
                      {"set_count", sets},
                      {"replacement_policy", policy},
                      {"random_seed", seed}});
  }
  return result;
}

namespace {
struct Session {
  std::string id, path, status = "preparing";
  U64 cursor = 0, revision = 0;
  bool busy = false;
  std::atomic<bool> cancelled{false};
  Json levels, total = nullptr, error = nullptr, metadata, last = nullptr;
  std::unique_ptr<CacheHierarchy> hierarchy;
  std::vector<ObservedLevel *> observed;
  std::unique_ptr<TraceReplay> replay;

  Json descriptor() const {
    return {{"session_id", id},
            {"trace_path", path},
            {"status", status},
            {"busy", busy},
            {"cursor", std::to_string(cursor)},
            {"revision", std::to_string(revision)},
            {"total_events", total},
            {"levels", levels},
            {"hierarchy", policies},
            {"limits", limits},
            {"error", error}};
  }
  ApiError failure(int http, const std::string &code,
                   const std::string &message) const {
    ApiError result(http, code, message);
    result.context = {{"session_id", id},
                      {"revision", std::to_string(revision)}};
    if (status == "failed")
      result.info["recovery"] = error.at("recovery");
    return result;
  }
  U64 next_revision() const {
    if (revision == std::numeric_limits<U64>::max())
      throw std::overflow_error("revision overflow");
    return revision + 1;
  }
  void advance_revision() { revision = next_revision(); }
  void failed(const std::string &code, const std::string &message,
              const std::string &recovery) {
    Json failure{{"code", code},
                 {"message", message},
                 {"details", Json::object()},
                 {"recovery", recovery}};
    advance_revision();
    status = "failed";
    busy = false;
    if (replay)
      cursor = replay->cursor();
    error = std::move(failure);
    last = nullptr;
  }
  Json progress() { return progress(cursor, revision, status, last); }
  Json progress(U64 at, U64 version, const std::string &state,
                const Json &event) {
    return {{"session_id", id},
            {"status", state},
            {"cursor", std::to_string(at)},
            {"revision", std::to_string(version)},
            {"summary", summary_dto(*replay)},
            {"last_event", event}};
  }
};
} // namespace

using Query = std::map<std::string, std::string>;
static Query query_values(const std::multimap<std::string, std::string> &input,
                          std::initializer_list<const char *> required,
                          std::initializer_list<const char *> optional = {}) {
  Query result;
  std::set<std::string> allowed(required.begin(), required.end());
  allowed.insert(optional.begin(), optional.end());
  for (const auto &[key, value] : input)
    if (!allowed.count(key) || !result.emplace(key, value).second)
      bad("invalid_request", "unknown or duplicate query parameter: " + key);
  for (const auto *key : required)
    if (!result.count(key))
      bad("invalid_request", std::string("missing query parameter: ") + key);
  return result;
}

static std::size_t page_number(const std::string &value, bool positive = true) {
  const auto parsed = decimal(value, "invalid_page");
  if (parsed > max_index || (positive && parsed == 0))
    bad("invalid_page", "page index or limit is out of range");
  return static_cast<std::size_t>(parsed);
}
} // namespace http_detail

using namespace http_detail;

struct RunnerApi::Impl {
  std::mutex mutex;
  std::condition_variable changed;
  std::shared_ptr<Session> active, pending;
  bool stopping = false;
  U64 serial = 0;
  std::string prefix;
  std::thread worker;
  RunnerApi::LevelFactory level_factory;

  explicit Impl(RunnerApi::LevelFactory factory)
      : level_factory(std::move(factory)) {
    if (!level_factory)
      level_factory = [](const CacheConfig &config,
                         const ReplacementPolicyFactory &policy) {
        return std::make_unique<SetAssociativeCacheLevel>(
            CacheLevelConfig{config.size_bytes, config.line_size_bytes,
                             config.associativity},
            policy);
      };
    std::random_device random;
    std::ostringstream name;
    for (int i = 0; i < 4; ++i)
      name << std::hex << std::setw(8) << std::setfill('0') << random();
    prefix = name.str();
    worker = std::thread([this] { prepare(); });
  }
  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
      if (active)
        active->cancelled = true;
      if (pending)
        pending->cancelled = true;
    }
    changed.notify_all();
    worker.join();
  }

  void prepare() {
    for (;;) {
      std::shared_ptr<Session> session;
      {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return stopping || pending; });
        if (stopping)
          return;
        session = std::move(pending);
      }
      std::string failure_code = "invalid_trace", recovery = "recreate",
                  message;
      try {
        failure_code = "trace_unreadable";
        if (!std::filesystem::is_regular_file(session->path))
          throw std::runtime_error("trace is not a regular file");
        std::ifstream input(session->path, std::ios::binary);
        if (!input)
          throw std::runtime_error("trace is unreadable");
        failure_code = "internal_error";
        std::vector<CacheLevel> levels;
        for (const auto &entry : session->levels) {
          const auto policy = entry.at("replacement_policy").get<std::string>();
          const auto seed =
              policy == "Random"
                  ? decimal(entry.at("random_seed"), "invalid_config")
                  : 0;
          ReplacementPolicyFactory factory =
              [policy, seed]() -> std::unique_ptr<IReplacementPolicy> {
            if (policy == "FIFO")
              return std::make_unique<FIFO>();
            if (policy == "Random")
              return std::make_unique<Random>(seed);
            return std::make_unique<LRU>();
          };
          CacheConfig config{entry.at("size_bytes").get<std::size_t>(),
                             entry.at("line_size_bytes").get<std::size_t>(),
                             entry.at("associativity").get<std::size_t>(),
                             policy};
          auto level = std::make_unique<ObservedLevel>(
              config, level_factory(config, factory));
          session->observed.push_back(level.get());
          levels.push_back(
              {entry.at("name").get<std::string>(), config, std::move(level)});
        }
        auto hierarchy = std::make_unique<CacheHierarchy>(std::move(levels));
        failure_code = "invalid_trace";
        auto replay =
            std::make_unique<TraceReplay>(session->path, *hierarchy, [session] {
              return session->cancelled.load();
            });
        auto metadata = metadata_dto(*replay);
        metadata["session_id"] = session->id;
        metadata["trace_path"] = session->path;
        replay->check_unchanged();
        std::lock_guard<std::mutex> lock(mutex);
        if (session->cancelled || active != session)
          continue;
        session->hierarchy = std::move(hierarchy);
        session->replay = std::move(replay);
        session->metadata = std::move(metadata);
        session->total = std::to_string(session->replay->total_events());
        session->status =
            session->replay->total_events() ? "ready" : "finished";
        session->advance_revision();
        continue;
      } catch (const TraceChanged &error) {
        failure_code = "trace_changed";
        message = error.what();
      } catch (const std::bad_alloc &error) {
        failure_code = "internal_error";
        message = error.what();
      } catch (const std::ios_base::failure &error) {
        failure_code = "internal_error";
        message = error.what();
      } catch (const std::exception &error) {
        message = error.what();
      }
      std::lock_guard<std::mutex> lock(mutex);
      if (!session->cancelled && active == session)
        session->failed(failure_code, message, recovery);
    }
  }

  std::shared_ptr<Session> find(const std::string &id) {
    if (!active || active->id != id)
      throw ApiError(404, "session_not_found", "session does not exist");
    return active;
  }
  void revision(Session &session, U64 expected) {
    if (session.revision != expected) {
      auto error = session.failure(409, "revision_mismatch",
                                   "published revision has changed");
      error.info["details"] = {
          {"expected_revision", std::to_string(expected)},
          {"actual_revision", std::to_string(session.revision)}};
      throw ApiError{std::move(error)};
    }
  }
  void available(Session &session, bool reset = false) {
    if (session.busy)
      throw session.failure(409, "session_busy",
                            "session is executing a command");
    if (session.status == "preparing")
      throw session.failure(409, "session_not_ready", "session is preparing");
    if (session.status == "failed" &&
        (!reset || session.error.at("recovery") != "reset"))
      throw session.failure(
          409, "session_failed",
          "session must be recovered before reading or stepping");
  }

  ApiResponse dispatch(const std::string &method, const std::string &path,
                       const std::multimap<std::string, std::string> &input,
                       const Json &body) {
    if (path == "/api/v1/sessions") {
      query_values(input, {});
      if (method == "POST") {
        auto configs = configuration(body);
        std::lock_guard<std::mutex> lock(mutex);
        if (active)
          throw active->failure(
              409, "session_exists",
              "delete the current session before creating another");
        if (serial == std::numeric_limits<U64>::max())
          throw std::overflow_error("session ID counter overflow");
        auto session = std::make_shared<Session>();
        session->id = prefix + "-" + std::to_string(++serial);
        session->path = body.at("trace_path").get<std::string>();
        session->levels = std::move(configs);
        active = pending = session;
        changed.notify_one();
        return {202, session->descriptor(), "/api/v1/sessions/" + session->id};
      }
      if (method == "GET") {
        std::lock_guard<std::mutex> lock(mutex);
        Json sessions = Json::array();
        if (active)
          sessions.push_back(active->descriptor());
        return {200, {{"sessions", std::move(sessions)}}, {}};
      }
    }
    const std::string prefix_path = "/api/v1/sessions/";
    if (path.compare(0, prefix_path.size(), prefix_path) != 0)
      throw ApiError(404, "session_not_found", "unknown API route");
    const auto tail = path.substr(prefix_path.size());
    const auto slash = tail.find('/');
    const auto id = tail.substr(0, slash);
    const auto resource =
        slash == std::string::npos ? "" : tail.substr(slash + 1);
    if (resource.empty() && method == "DELETE") {
      auto query = query_values(input, {"expected_revision"});
      const auto expected =
          decimal(query.at("expected_revision"), "invalid_request");
      std::lock_guard<std::mutex> lock(mutex);
      auto session = find(id);
      revision(*session, expected);
      if (session->busy)
        throw session->failure(409, "session_busy",
                               "session is executing a command");
      session->cancelled = true;
      if (pending == session)
        pending.reset();
      active.reset();
      return {204, nullptr, {}};
    }
    if (resource.empty() && method == "GET") {
      query_values(input, {});
      std::lock_guard<std::mutex> lock(mutex);
      return {200, find(id)->descriptor(), {}};
    }
    if (resource == "metadata" && method == "GET") {
      query_values(input, {});
      std::lock_guard<std::mutex> lock(mutex);
      auto session = find(id);
      if (session->status == "preparing")
        throw session->failure(409, "session_not_ready",
                               "session is preparing");
      if (session->status == "failed")
        throw session->failure(409, "session_failed", "session has failed");
      return {200, session->metadata, {}};
    }
    if ((resource == "step" || resource == "reset") && method == "POST") {
      query_values(input, {});
      const bool reset = resource == "reset";
      if (reset)
        keys(body, {"expected_revision"});
      else
        keys(body, {"expected_revision"}, {"count"});
      const auto expected =
          decimal(body.at("expected_revision"), "invalid_request");
      const auto count = reset ? 0
                         : body.contains("count")
                             ? number(body.at("count"), "invalid_count")
                             : 1;
      if (count > limits.at("max_step_count").get<std::size_t>())
        bad("invalid_count", "count exceeds max_step_count");
      std::shared_ptr<Session> session;
      {
        std::lock_guard<std::mutex> lock(mutex);
        session = find(id);
        revision(*session, expected);
        available(*session, reset);
        session->busy = true;
      }
      Json outcomes = Json::array();
      try {
        session->replay->check_unchanged();
        if (reset) {
          session->replay->reset();
        } else {
          for (std::size_t i = 0;
               i < count &&
               session->replay->cursor() < session->replay->total_events();
               ++i)
            outcomes.push_back(outcome_dto(session->replay->step()));
        }
        session->replay->check_unchanged();
        std::lock_guard<std::mutex> lock(mutex);
        const auto next_cursor = session->replay->cursor();
        const auto next_revision = reset || !outcomes.empty()
                                       ? session->next_revision()
                                       : session->revision;
        std::string state = next_cursor == session->replay->total_events()
                                ? "finished"
                                : "ready";
        Json last = reset              ? Json(nullptr)
                    : outcomes.empty() ? session->last
                                       : outcomes.back();
        auto result =
            session->progress(next_cursor, next_revision, state, last);
        if (!reset) {
          result["executed_count"] = outcomes.size();
          result["events"] = std::move(outcomes);
        }
        session->cursor = next_cursor;
        session->revision = next_revision;
        session->status = std::move(state);
        session->last = std::move(last);
        session->error = nullptr;
        session->busy = false;
        return {200, std::move(result), {}};
      } catch (const TraceChanged &error) {
        outcomes.clear();
        std::lock_guard<std::mutex> lock(mutex);
        session->failed("trace_changed", error.what(), "recreate");
        throw session->failure(500, "trace_changed", error.what());
      } catch (const std::exception &error) {
        outcomes.clear();
        std::lock_guard<std::mutex> lock(mutex);
        session->failed("execution_failed", error.what(), "reset");
        throw session->failure(500, "execution_failed", error.what());
      }
    }
    if ((resource == "state" || resource == "instructions") &&
        method == "GET") {
      const bool state = resource == "state";
      const auto query =
          state
              ? query_values(input, {"expected_revision", "level", "set_limit"},
                             {"set_offset"})
              : query_values(input, {"expected_revision", "limit"}, {"offset"});
      const auto expected =
          decimal(query.at("expected_revision"), "invalid_request");
      const auto limit = page_number(query.at(state ? "set_limit" : "limit"));
      const auto offset =
          query.count(state ? "set_offset" : "offset")
              ? decimal(query.at(state ? "set_offset" : "offset"),
                        "invalid_page")
              : 0;
      const auto level = state ? page_number(query.at("level")) : 0;
      std::lock_guard<std::mutex> lock(mutex);
      auto session = find(id);
      revision(*session, expected);
      available(*session);
      if (limit >
          limits.at(state ? "max_sets_per_read" : "max_instructions_per_read")
              .get<std::size_t>())
        bad("invalid_page", "page limit exceeds negotiated maximum");
      if (state) {
        if (level > session->levels.size())
          bad("invalid_page", "unknown cache level");
        const auto total =
            session->levels.at(level - 1).at("set_count").get<std::size_t>();
        if (offset > total)
          bad("invalid_page", "set_offset exceeds set_count");
        auto sets = session->observed.at(level - 1)->page(
            static_cast<std::size_t>(offset), limit);
        const auto next = offset + sets.size();
        auto result = session->progress();
        result["cache"] = {
            {"level", level},
            {"set_offset", offset},
            {"set_limit", limit},
            {"total_sets", total},
            {"next_set_offset", next < total ? Json(next) : Json(nullptr)},
            {"sets", std::move(sets)}};
        return {200, std::move(result), {}};
      }
      const auto total = session->replay->summary().instructions.size();
      if (offset > total)
        bad("invalid_page", "instruction offset exceeds row count");
      auto rows = session->replay->instructions(offset, limit);
      for (auto &row : rows) {
        auto normalized = access_dto(row);
        normalized["events"] = row.at("events").dump();
        normalized["requested_bytes"] = row.at("requested_bytes").dump();
        normalized["levels"] = row.at("levels");
        for (auto &entry : normalized.at("levels"))
          level_statistics(entry);
        row = std::move(normalized);
      }
      const auto next = offset + rows.size();
      return {200,
              {{"session_id", session->id},
               {"cursor", std::to_string(session->cursor)},
               {"revision", std::to_string(session->revision)},
               {"offset", std::to_string(offset)},
               {"limit", limit},
               {"total_instructions", std::to_string(total)},
               {"next_offset",
                next < total ? Json(std::to_string(next)) : Json(nullptr)},
               {"instructions", std::move(rows)}},
              {}};
    }
    throw ApiError(404, "session_not_found", "unknown API route");
  }
};

RunnerApi::RunnerApi(LevelFactory factory)
    : impl_(std::make_unique<Impl>(std::move(factory))) {}
RunnerApi::~RunnerApi() = default;
ApiResponse
RunnerApi::handle(const std::string &method, const std::string &path,
                  const std::multimap<std::string, std::string> &query,
                  const Json &body) {
  try {
    return impl_->dispatch(method, path, query, body);
  } catch (const ApiError &error) {
    auto result = error.context;
    if (result.empty()) {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      if (impl_->active) {
        const auto base = "/api/v1/sessions/" + impl_->active->id;
        if (path == base || path.compare(0, base.size() + 1, base + '/') == 0) {
          result["session_id"] = impl_->active->id;
          result["revision"] = std::to_string(impl_->active->revision);
        }
      }
    }
    result["error"] = error.info;
    return {error.status, std::move(result), {}};
  } catch (const std::exception &error) {
    return {500,
            {{"error",
              {{"code", "internal_error"},
               {"message", error.what()},
               {"details", Json::object()}}}},
            {}};
  }
}
} // namespace cache_simulator
