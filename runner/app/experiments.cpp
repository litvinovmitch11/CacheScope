#include "experiments.hpp"
#include "cache_simulator/cache_policies.hpp"
#include "http_json.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;

namespace cache_simulator {
namespace {
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct RequestError : std::runtime_error {
  int status;
  RequestError(int status, const std::string &message)
      : std::runtime_error(message), status(status) {}
};
Json read(const fs::path &path) {
  std::ifstream input(path);
  Json value;
  input >> value;
  return value;
}
void send(httplib::Response &response, int status, const Json &body) {
  response.status = status;
  response.set_content(
      body.dump(-1, ' ', false, Json::error_handler_t::replace),
      "application/json");
}
void require(bool condition, const std::string &message) {
  if (!condition)
    throw RequestError(400, message);
}
std::uint64_t decimal(const Json &value, bool positive = true) {
  require(value.is_string(), "Expected uint64 decimal string");
  const auto text = value.get<std::string>();
  std::uint64_t result = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), result);
  require(!text.empty() && (text.size() == 1 || text[0] != '0') &&
              parsed.ec == std::errc{} &&
              parsed.ptr == text.data() + text.size() &&
              (!positive || result > 0),
          "Invalid uint64 decimal string");
  return result;
}
std::uint64_t number(const Json &value) {
  require(value.is_number_integer() && value > 0 &&
              value <= 9007199254740991ULL,
          "Expected positive safe integer");
  return value.get<std::uint64_t>();
}
void validate(const Json &spec, const Json &metadata) {
  require(
      spec.is_object() &&
          spec.size() ==
              5 + spec.count("hierarchy") + spec.count("cache_profile") &&
          spec.contains("function") && spec.contains("bindings") &&
          spec.contains("levels") && spec.contains("timeout_seconds") &&
          spec.contains("trace_limit"),
      "Expected function, bindings, levels, timeout_seconds and trace_limit");
  if (spec.contains("cache_profile"))
    require(spec.at("cache_profile").is_string() &&
                spec.at("cache_profile").get<std::string>().size() <= 128,
            "cache_profile must be a string of at most 128 bytes");
  const auto &functions = metadata.at("functions");
  const auto selected =
      std::find_if(functions.begin(), functions.end(), [&](const auto &f) {
        return f.at("name") == spec.at("function");
      });
  require(selected != functions.end() && selected->at("supported") == true,
          "Unsupported or unknown function");
  const auto &bindings = spec.at("bindings");
  require(bindings.is_array() &&
              bindings.size() == selected->at("arguments").size(),
          "Provide one binding for each function argument");
  for (std::size_t i = 0; i < bindings.size(); ++i) {
    const auto &binding = bindings[i];
    require(binding.is_object() && binding.contains("source"),
            "Invalid argument binding");
    if (binding.at("source") == "zeros") {
      require(binding.size() == 1, "Unexpected zero binding fields");
      continue;
    }
    require(binding.size() == 2 && binding.at("source") == "npz" &&
                binding.contains("key"),
            "Expected NPZ key or zero initialization");
    const auto &arrays = metadata.at("arrays");
    const auto array =
        std::find_if(arrays.begin(), arrays.end(), [&](const auto &a) {
          return a.at("key") == binding.at("key");
        });
    require(array != arrays.end() && array->at("dtype") == "float32" &&
                array->at("shape") ==
                    selected->at("arguments").at(i).at("shape"),
            "Tensor dtype or shape does not match the argument");
  }
  if (!spec.at("timeout_seconds").is_null())
    require(spec.at("timeout_seconds").is_number() &&
                std::isfinite(spec.at("timeout_seconds").get<double>()) &&
                spec.at("timeout_seconds") > 0,
            "timeout_seconds must be positive or null");
  if (!spec.at("trace_limit").is_null())
    decimal(spec.at("trace_limit"));
  const auto &levels = spec.at("levels");
  require(levels.is_array() && !levels.empty(),
          "Provide at least one cache level");
  std::set<std::string> names;
  std::uint64_t common_line = 0;
  for (const auto &level : levels) {
    require(level.is_object() && level.contains("name") &&
                level.at("name").is_string() && level.contains("size_bytes") &&
                level.contains("line_size_bytes") &&
                level.contains("associativity") &&
                level.contains("replacement_policy"),
            "Invalid cache level");
    const auto name = level.at("name").get<std::string>();
    const auto size = number(level.at("size_bytes"));
    const auto line = number(level.at("line_size_bytes"));
    const auto ways = number(level.at("associativity"));
    const auto sets = size / line / ways;
    require(!name.empty() && name.find('\0') == std::string::npos &&
                names.insert(name).second && !(line & (line - 1)) && sets &&
                !(sets & (sets - 1)) && size % line == 0 &&
                (size / line) % ways == 0 &&
                (!common_line || common_line == line),
            "Invalid cache geometry or name");
    common_line = line;
    const auto &policy = level.at("replacement_policy");
    require(policy == "LRU" || policy == "FIFO" || policy == "Random" ||
                policy == "PLRU4",
            "Unknown cache policy");
    require(policy != "PLRU4" || ways == 4, "PLRU4 requires exactly four ways");
    require(level.size() == 5 + level.count("random_seed") +
                                level.count("write_policy") +
                                level.count("allocation_policy"),
            "Unknown cache configuration field");
    CacheConfig config{};
    configure_policies(config, level);
    if (level.contains("random_seed")) {
      require(policy == "Random", "Only Random accepts random_seed");
      decimal(level.at("random_seed"), false);
    }
  }
  parse_hierarchy_policies(spec.value("hierarchy", Json::object()),
                           levels.size());
}

void validate_template_spec(const Json &spec) {
  require(spec.is_object() && spec.size() == 3 &&
              spec.contains("template_id") && spec.contains("parameters") &&
              spec.contains("timeout_seconds"),
          "Expected template_id, parameters and timeout_seconds");
  require(spec.at("template_id") == "depthwise-temporal-blocking",
          "Unknown experiment template");
  const auto &parameters = spec.at("parameters");
  require(parameters.is_object() && parameters.size() == 2 &&
              parameters.contains("tile_sizes") &&
              parameters.contains("stepper_events"),
          "Expected tile_sizes and stepper_events parameters");
  const auto &tiles = parameters.at("tile_sizes");
  require(tiles.is_array() && !tiles.empty() && tiles.size() <= 8,
          "tile_sizes must contain between 1 and 8 values");
  std::set<std::uint64_t> unique_tiles;
  for (const auto &tile : tiles) {
    const auto value = number(tile);
    require(value <= 256 && unique_tiles.insert(value).second,
            "tile_sizes must be unique integers between 1 and 256");
  }
  require(parameters.at("stepper_events").is_number_unsigned() &&
              parameters.at("stepper_events").get<std::uint64_t>() <= 10000,
          "stepper_events must be an integer between 0 and 10000");
  if (!spec.at("timeout_seconds").is_null())
    require(spec.at("timeout_seconds").is_number() &&
                std::isfinite(spec.at("timeout_seconds").get<double>()) &&
                spec.at("timeout_seconds") > 0 &&
                spec.at("timeout_seconds") <= 7200,
            "timeout_seconds must be between 0 and 7200 seconds or null");
}

void validate_project_spec(const Json &spec) {
  require(spec.is_object() && spec.size() == 7 && spec.contains("schema") &&
              spec.at("schema") == "cachescope.torch-project.v1" &&
              spec.contains("name") && spec.contains("files") &&
              spec.contains("variants") && spec.contains("cache_profiles") &&
              spec.contains("settings") && spec.contains("timeout_seconds"),
          "Expected a cachescope.torch-project.v1 project");
  require(spec.at("name").is_string() && !spec.at("name").get<std::string>().empty() &&
              spec.at("name").get<std::string>().size() <= 128,
          "Project name must contain between 1 and 128 characters");
  const auto &files = spec.at("files");
  require(files.is_object() && files.size() == 2 && files.contains("model.py") &&
              files.contains("inputs.py"),
          "Project files must contain model.py and inputs.py");
  for (const auto &item : files.items())
    require(item.value().is_string() && !item.value().get<std::string>().empty() &&
                item.value().get<std::string>().size() <= 262144,
            item.key() + " must be non-empty and at most 256 KiB");
  const auto &variants = spec.at("variants");
  require(variants.is_array() && !variants.empty() && variants.size() <= 12,
          "Provide between 1 and 12 schedules");
  std::set<std::string> names;
  for (const auto &variant : variants) {
    require(variant.is_object() && variant.size() == 2 &&
                variant.contains("name") && variant.contains("transform") &&
                variant.at("name").is_string() &&
                variant.at("transform").is_string(),
            "Each schedule requires name and transform");
    const auto name = variant.at("name").get<std::string>();
    require(!name.empty() && name.size() <= 64 && names.insert(name).second &&
                std::all_of(name.begin(), name.end(), [](unsigned char value) {
                  return std::isalnum(value) || value == '-' || value == '_';
                }),
            "Schedule names must be unique identifiers");
    require(!variant.at("transform").get<std::string>().empty() &&
                variant.at("transform").get<std::string>().size() <= 262144,
            "Transform source must be non-empty and at most 256 KiB");
  }
  const auto &profiles = spec.at("cache_profiles");
  require(profiles.is_object() && !profiles.empty() && profiles.size() <= 8,
          "Provide between 1 and 8 cache profiles");
  for (const auto &profile : profiles.items()) {
    require(!profile.key().empty() && profile.key().size() <= 64 &&
                std::all_of(profile.key().begin(), profile.key().end(),
                            [](unsigned char value) {
                              return std::isalnum(value) || value == '-' ||
                                     value == '_';
                            }),
            "Cache profile names must be identifiers");
    if (profile.value().is_array()) {
      require(!profile.value().empty() && profile.value().size() <= 8,
              "Each cache profile requires between 1 and 8 levels");
      for (const auto &level : profile.value())
        require(level.is_string() && !level.get<std::string>().empty() &&
                    level.get<std::string>().size() <= 128,
                "Compact cache levels must use runner strings");
      continue;
    }
    const auto &configuration = profile.value();
    require(configuration.is_object() && configuration.contains("levels") &&
                configuration.size() ==
                    1 + configuration.count("hierarchy") &&
                (!configuration.contains("hierarchy") ||
                 configuration.at("hierarchy").is_object()),
            "Structured cache profiles require levels and optional hierarchy");
    const auto &levels = configuration.at("levels");
    require(levels.is_array() && !levels.empty() && levels.size() <= 8,
            "Each structured cache profile requires 1 to 8 levels");
    for (const auto &level : levels) {
      require(level.is_object() && level.contains("name") &&
                  level.contains("size_bytes") &&
                  level.contains("line_size_bytes") &&
                  level.contains("associativity") &&
                  level.contains("replacement_policy") &&
                  level.at("name").is_string() &&
                  !level.at("name").get<std::string>().empty(),
              "Structured cache levels require geometry and policy");
      require(level.size() == 5 + level.count("random_seed") +
                                      level.count("write_policy") +
                                      level.count("allocation_policy"),
              "Unknown structured cache level field");
      const auto size = number(level.at("size_bytes"));
      const auto line = number(level.at("line_size_bytes"));
      const auto ways = number(level.at("associativity"));
      const auto policy = level.at("replacement_policy");
      require((policy == "LRU" || policy == "FIFO" || policy == "Random" ||
               policy == "PLRU4") &&
                  size % line == 0 && (size / line) % ways == 0 &&
                  (policy != "PLRU4" || ways == 4),
              "Invalid structured cache geometry or replacement policy");
      if (level.contains("write_policy"))
        require(level.at("write_policy") == "write_back" ||
                    level.at("write_policy") == "write_through",
                "Unknown write policy");
      if (level.contains("allocation_policy"))
        require(level.at("allocation_policy") == "write_allocate" ||
                    level.at("allocation_policy") == "no_write_allocate",
                "Unknown allocation policy");
      if (level.contains("random_seed")) {
        require(policy == "Random", "Only Random accepts random_seed");
        decimal(level.at("random_seed"), false);
      }
    }
  }
  const auto &settings = spec.at("settings");
  require(settings.is_object() && settings.size() == 3 &&
              settings.contains("stepper_events") &&
              settings.contains("trace_limit") && settings.contains("validation"),
          "Expected stepper_events, trace_limit and validation settings");
  require(settings.at("stepper_events").is_number_unsigned() &&
              settings.at("stepper_events").get<std::uint64_t>() <= 10000,
          "stepper_events must be between 0 and 10000");
  require(settings.at("trace_limit").is_number_unsigned() &&
              settings.at("trace_limit").get<std::uint64_t>() >= 1000 &&
              settings.at("trace_limit").get<std::uint64_t>() <= 10000000,
          "trace_limit must be between 1000 and 10000000");
  const auto &validation = settings.at("validation");
  require(validation.is_object() && validation.size() == 2 &&
              validation.contains("rtol") && validation.contains("atol") &&
              validation.at("rtol").is_number() &&
              validation.at("atol").is_number() &&
              validation.at("rtol").get<double>() >= 0 &&
              validation.at("atol").get<double>() >= 0,
          "Validation requires non-negative rtol and atol");
  if (!spec.at("timeout_seconds").is_null())
    require(spec.at("timeout_seconds").is_number() &&
                spec.at("timeout_seconds").get<double>() > 0 &&
                spec.at("timeout_seconds").get<double>() <= 7200,
            "timeout_seconds must be between 0 and 7200 seconds or null");
}

struct Job {
  fs::path directory;
  std::string id, status = "preparing", template_id;
  Json metadata = nullptr, spec = nullptr, error = nullptr;
  std::atomic<bool> cancelled{false};
  pid_t pid = 0;
  ~Job() {
    std::error_code ignored;
    fs::remove_all(directory, ignored);
  }
  bool active() const {
    return status == "preparing" || status == "running" ||
           status == "cancelling";
  }
  Json descriptor() const {
    Json progress = nullptr;
    try {
      progress = read(directory / "progress.json");
    } catch (const std::exception &) {
    }
    Json phase = progress.is_object() && progress.contains("stage")
                     ? progress.at("stage")
                     : Json(nullptr);
    if (status == "ready" || status == "completed")
      phase = nullptr;
    Json artifacts = nullptr;
    if (!template_id.empty() && status == "completed") {
      const auto root = "/api/v1/experiments/" + id + "/artifacts/";
      const auto report = template_id == "local-torch-project"
                              ? "/?view=report&experiment=" + id
                              : root + "report";
      artifacts = {{"report_url", report},
                   {"summary_url", root + "summary"},
                   {"stepper_url", root + "stepper"}};
    }
    Json visible_spec = spec;
    if (template_id == "local-torch-project" && visible_spec.is_object()) {
      visible_spec.erase("files");
      if (visible_spec.contains("variants"))
        for (auto &variant : visible_spec["variants"])
          variant.erase("transform");
    }
    return {{"experiment_id", id},
            {"template_id", template_id.empty() ? Json(nullptr)
                                                  : Json(template_id)},
            {"status", status},
            {"stage", phase},
            {"progress", progress},
            {"metadata", metadata},
            {"spec", visible_spec},
            {"artifacts", artifacts},
            {"error", error}};
  }
};
} // namespace

struct Experiments::Impl {
  fs::path app;
  std::string allowed_origin;
  std::mutex mutex;
  std::shared_ptr<Job> current;
  std::thread worker;

  Impl(fs::path app, std::string allowed_origin)
      : app(std::move(app)), allowed_origin(std::move(allowed_origin)) {}
  fs::path template_build_dir() const {
    return fs::is_regular_file(app / "build/app/runner/cachescope-runner")
               ? app / "build/app"
               : app / "build";
  }
  bool template_ready() const {
    const auto configured = std::getenv("CACHESCOPE_PYTHON");
    const bool python = (configured && *configured) ||
                        fs::is_regular_file(app / "venv/bin/python");
    const auto build = template_build_dir();
    return python &&
           fs::is_regular_file(
               app / "examples/ml-locality/depthwise-temporal-blocking/run.py") &&
           fs::is_regular_file(build / "lib/CacheScopePassPlugin.so") &&
           fs::is_regular_file(
               build / "compiler/runtime/libcachescope_trace_runtime.so") &&
           fs::is_regular_file(build / "runner/cachescope-runner");
  }
  void check_origin(const httplib::Request &request) const {
    const auto origin = request.get_header_value("Origin");
    if (!origin.empty() && origin != allowed_origin &&
        origin != "http://" + request.get_header_value("Host"))
      throw RequestError(
          403, "Origin is not allowed to start or cancel local experiments");
  }
  ~Impl() {
    {
      std::lock_guard lock(mutex);
      if (current)
        current->cancelled = true;
    }
    if (worker.joinable())
      worker.join();
  }
  void start(const std::shared_ptr<Job> &job, const std::string &mode) {
    if (worker.joinable())
      worker.join();
    {
      std::ofstream output(job->directory / "progress.tmp");
      output << Json{
          {"stage", mode == "prepare" ? "inspection"
                                      : mode == "template" ? "preparation"
                                                           : "compilation"}};
      output.close();
      if (output.fail())
        throw std::runtime_error("Cannot publish experiment stage");
      fs::rename(job->directory / "progress.tmp",
                 job->directory / "progress.json");
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                     (job->directory / "worker.log").c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t empty;
    sigemptyset(&empty);
    posix_spawnattr_setsigmask(&attributes, &empty);
    posix_spawnattr_setflags(&attributes,
                             POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
    posix_spawnattr_setpgroup(&attributes, 0);
    std::vector<std::string> arguments;
    if (mode == "template") {
      const auto build = template_build_dir();
      std::string tiles;
      for (const auto &tile : job->spec.at("parameters").at("tile_sizes")) {
        if (!tiles.empty())
          tiles += ',';
        tiles += std::to_string(tile.get<std::uint64_t>());
      }
      const auto configured_python = std::getenv("CACHESCOPE_PYTHON");
      const auto bundled_python = app / "venv/bin/python";
      const std::string python = configured_python && *configured_python
                                     ? configured_python
                                     : fs::is_regular_file(bundled_python)
                                           ? bundled_python.string()
                                           : "python3";
      arguments = {
          python,
          (app / "examples/ml-locality/depthwise-temporal-blocking/run.py")
              .string(),
          "--skip-build",
          "--build-dir",
          build.string(),
          "--output-dir",
          (job->directory / "output").string(),
          "--tile-sizes",
          tiles,
          "--stepper-events",
          std::to_string(job->spec.at("parameters")
                             .at("stepper_events")
                             .get<std::uint64_t>()),
          "--progress-file",
          (job->directory / "progress.json").string(),
      };
    } else if (mode == "project") {
      const auto configured_python = std::getenv("CACHESCOPE_PYTHON");
      const auto bundled_python = app / "venv/bin/python";
      const std::string python = configured_python && *configured_python
                                     ? configured_python
                                     : bundled_python.string();
      arguments = {python, (app / "compiler/torch_project_worker.py").string(),
                   job->directory.string()};
    } else {
      arguments = {"python3",
                   (app / "compiler/experiment_worker.py").string(), mode,
                   "--job", job->directory.string()};
    }
    std::vector<char *> argv;
    for (auto &argument : arguments)
      argv.push_back(argument.data());
    argv.push_back(nullptr);
    const auto result = posix_spawnp(&job->pid, arguments.front().c_str(),
                                     &actions, &attributes, argv.data(),
                                     environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (result != 0)
      throw std::system_error(result, std::generic_category(),
                              "start compiler worker");
    job->status = mode == "prepare" ? "preparing" : "running";
    try {
      worker = std::thread([this, job, mode] { monitor(job, mode); });
    } catch (...) {
      kill(-job->pid, SIGKILL);
      waitpid(job->pid, nullptr, 0);
      job->pid = 0;
      throw;
    }
  }
  void monitor(const std::shared_ptr<Job> &job, const std::string &mode) {
    const auto begin = Clock::now();
    const auto pid = job->pid;
    bool timed_out = false, stopping = false;
    auto stopped_at = begin;
    int status = 0;
    for (;;) {
      const auto result = waitpid(pid, &status, WNOHANG);
      if (result == pid)
        break;
      if (result < 0) {
        if (errno == EINTR)
          continue;
        status = 1 << 8;
        break;
      }
      if (mode != "prepare" && !job->spec.at("timeout_seconds").is_null() &&
          std::chrono::duration<double>(Clock::now() - begin).count() >=
              job->spec.at("timeout_seconds").get<double>())
        timed_out = true;
      if (!stopping && (job->cancelled || timed_out)) {
        stopping = true;
        stopped_at = Clock::now();
        kill(-pid, SIGTERM);
      }
      if (stopping && Clock::now() - stopped_at > std::chrono::seconds(2))
        kill(-pid, SIGKILL);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Reap any subprocess group left behind by a crashed worker.
    kill(-pid, SIGKILL);
    std::lock_guard lock(mutex);
    job->pid = 0;
    try {
      if (job->cancelled) {
        job->status = "cancelled";
        std::error_code ignored;
        fs::remove_all(job->directory, ignored);
      } else if (timed_out) {
        job->status = "failed";
        job->error = {{"code", "timeout"},
                      {"message", "Experiment timeout exceeded"}};
      } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        if (mode == "prepare")
          job->metadata = read(job->directory / "metadata.json");
        else if (mode == "template" || mode == "project") {
          for (const auto &artifact : {"summary.json", "ui-results.json"})
            if (!fs::is_regular_file(job->directory / "output" / artifact))
              throw std::runtime_error("Template worker produced no " +
                                       std::string(artifact));
          if (mode == "template") {
            if (!fs::is_regular_file(job->directory / "output/report.html"))
              throw std::runtime_error("Template worker produced no report.html");
            if (!fs::is_regular_file(job->directory /
                                     "output/stepper-demo/trace.json"))
              throw std::runtime_error("Worker produced no Stepper artifact");
          } else if (job->spec.at("settings").at("stepper_events") != 0 &&
                     !fs::is_directory(job->directory / "output/stepper")) {
            throw std::runtime_error("Worker produced no Stepper artifacts");
          }
        } else if (!fs::is_regular_file(job->directory / "report.json")) {
          throw std::runtime_error("Worker produced no complete report");
        }
        job->status = mode == "prepare" ? "ready" : "completed";
      } else {
        job->status = "failed";
        if (fs::is_regular_file(job->directory / "error.json"))
          job->error = read(job->directory / "error.json");
        else
          job->error = {{"code", "execution_failed"},
                        {"message", "Compiler worker terminated unexpectedly"}};
        std::ifstream log(job->directory / "worker.log",
                          std::ios::binary | std::ios::ate);
        const auto end = log.tellg();
        if (end > 0) {
          log.seekg(std::max<std::streamoff>(
              0, static_cast<std::streamoff>(end) - 16384));
          job->error["diagnostic"] =
              std::string(std::istreambuf_iterator<char>(log), {});
        }
      }
    } catch (const std::exception &error) {
      job->status = "failed";
      job->error = {{"code", "internal_error"}, {"message", error.what()}};
    }
  }
  template <typename Function>
  void guarded(httplib::Response &response, Function function) {
    try {
      if (app.empty())
        throw RequestError(503, "Start the full application with --app-dir");
      function();
    } catch (const RequestError &error) {
      send(response, error.status,
           {{"error",
             {{"code", error.status == 409   ? "experiment_conflict"
                       : error.status == 404 ? "experiment_not_found"
                       : error.status == 503 ? "worker_unavailable"
                                             : "invalid_request"},
              {"message", error.what()}}}});
    } catch (const nlohmann::json::exception &error) {
      send(response, 400,
           {{"error",
             {{"code", "invalid_request"}, {"message", error.what()}}}});
    } catch (const std::invalid_argument &error) {
      send(response, 400,
           {{"error",
             {{"code", "invalid_request"}, {"message", error.what()}}}});
    } catch (const std::exception &error) {
      send(
          response, 500,
          {{"error", {{"code", "internal_error"}, {"message", error.what()}}}});
    }
  }
  std::shared_ptr<Job> find(const std::string &id) {
    if (!current || current->id != id)
      throw RequestError(404, "Experiment was not found or has been replaced");
    return current;
  }
  std::shared_ptr<Job> reserve_job() {
    auto job = std::make_shared<Job>();
    std::lock_guard lock(mutex);
    if (current && current->active())
      throw RequestError(409, "An experiment is already active");
    std::string pattern =
        (fs::temp_directory_path() / "cachescope-experiment-XXXXXX").string();
    if (!mkdtemp(pattern.data()))
      throw std::runtime_error("Cannot create experiment directory");
    job->directory = pattern;
    job->id = job->directory.filename().string();
    current = job;
    return job;
  }
  void create_template(const httplib::Request &request,
                       httplib::Response &response,
                       const httplib::ContentReader &reader) {
    guarded(response, [&] {
      check_origin(request);
      require(json_content_type(request.get_header_value("Content-Type")),
              "Expected application/json");
      std::string body;
      const bool received = reader([&](const char *data, std::size_t size) {
        if (body.size() + size > 4 * 1024 * 1024)
          return false;
        body.append(data, size);
        return true;
      });
      require(received, "Experiment specification is too large");
      const auto spec = parse_api_json(body);
      const bool project = spec.is_object() && spec.value("schema", "") ==
                                                  "cachescope.torch-project.v1";
      if (project)
        validate_project_spec(spec);
      else
        validate_template_spec(spec);
      if (!template_ready())
        throw RequestError(
            503,
            "Torch experiment worker is unavailable: build CacheScope and "
            "install requirements-torch-demo.lock into venv, or set "
            "CACHESCOPE_PYTHON");
      auto job = reserve_job();
      job->template_id = project ? "local-torch-project"
                                 : spec.at("template_id").get<std::string>();
      job->spec = spec;
      try {
        if (project) {
          fs::create_directories(job->directory / "project/transforms");
          for (const auto &file : spec.at("files").items()) {
            std::ofstream source(job->directory / "project" / file.key());
            source << file.value().get<std::string>();
            if (!source.good())
              throw std::runtime_error("Cannot save project source");
          }
          Json normalized = spec;
          normalized.erase("files");
          normalized["runtime"] = {
              {"build_dir", template_build_dir().string()},
              {"llvm_root", std::getenv("LLVM_ROOT")
                                ? std::getenv("LLVM_ROOT")
                                : "/usr/lib/llvm-22"}};
          for (std::size_t index = 0; index < normalized["variants"].size(); ++index) {
            const auto name = normalized["variants"][index]["name"].get<std::string>();
            const auto path = "transforms/" + name + ".mlir";
            std::ofstream transform(job->directory / "project" / path);
            transform << normalized["variants"][index]["transform"].get<std::string>();
            if (!transform.good())
              throw std::runtime_error("Cannot save MLIR transform");
            normalized["variants"][index].erase("transform");
            normalized["variants"][index]["path"] = path;
          }
          std::ofstream project_file(job->directory / "project.json");
          project_file << normalized;
          if (!project_file.good())
            throw std::runtime_error("Cannot save project manifest");
        }
        std::ofstream output(job->directory / "spec.json");
        output << spec;
        if (!output.good())
          throw std::runtime_error("Cannot save experiment parameters");
        start(job, project ? "project" : "template");
        response.set_header("Location", "/api/v1/experiments/" + job->id);
        send(response, 202, job->descriptor());
      } catch (...) {
        job->status = "failed";
        job->error = {{"code", "execution_failed"},
                      {"message", "Cannot start experiment template"}};
        throw;
      }
    });
  }
  void upload(const httplib::Request &request, httplib::Response &response,
              const httplib::ContentReader &reader) {
    guarded(response, [&] {
      check_origin(request);
      require(request.is_multipart_form_data(),
              "Expected multipart MLIR and NPZ files");
      auto job = reserve_job();
      try {
        std::set<std::string> fields;
        std::ofstream output;
        const bool ok = reader(
            [&](const httplib::FormData &part) {
              if (output.is_open()) {
                output.close();
                if (output.fail())
                  return false;
              }
              if ((part.name != "mlir" && part.name != "inputs") ||
                  !fields.insert(part.name).second)
                return false;
              output.open(job->directory / (part.name == "mlir" ? "input.mlir"
                                                                : "inputs.npz"),
                          std::ios::binary);
              return output.good() && !job->cancelled;
            },
            [&](const char *data, std::size_t size) {
              output.write(data, static_cast<std::streamsize>(size));
              return output.good() && !job->cancelled;
            });
        if (output.is_open())
          output.close();
        require(ok && !output.fail() && fields.size() == 2,
                "Incomplete upload; provide mlir and inputs once each");
        std::lock_guard lock(mutex);
        if (job->cancelled)
          job->status = "cancelled";
        else
          start(job, "prepare");
        response.set_header("Location", "/api/v1/experiments/" + job->id);
        send(response, 202, job->descriptor());
      } catch (...) {
        std::lock_guard lock(mutex);
        job->status = job->cancelled ? "cancelled" : "failed";
        job->error = {{"code", "invalid_input"},
                      {"message", "Upload or preparation failed"}};
        if (job->cancelled) {
          std::error_code ignored;
          fs::remove_all(job->directory, ignored);
        }
        throw;
      }
    });
  }
};

Experiments::Experiments(httplib::Server &server, const fs::path &app,
                         const std::string &allowed_origin)
    : impl_(std::make_unique<Impl>(app, allowed_origin)) {
  auto *impl = impl_.get();
  server.Get("/api/v1/experiment-templates",
             [impl](const auto &, auto &response) {
               impl->guarded(response, [&] {
                 Json parameters = {
                     {"tile_sizes",
                      {{"type", "integer-list"},
                       {"default", Json::array({6, 10, 14})},
                       {"minimum", 1},
                       {"maximum", 256},
                       {"max_items", 8}}},
                     {"stepper_events",
                      {{"type", "integer"},
                       {"default", 2048},
                       {"minimum", 0},
                       {"maximum", 10000}}}};
                 Json item = {
                     {"template_id", "depthwise-temporal-blocking"},
                     {"name", "Depthwise temporal blocking"},
                     {"description",
                      "Compare baseline and tiled schedules of one two-layer "
                      "Torch depthwise convolution."},
                     {"parameters", std::move(parameters)},
                     {"available", impl->template_ready()},
                     {"unavailable_reason",
                      impl->template_ready()
                          ? Json(nullptr)
                          : Json("Install the pinned Torch environment and "
                                 "build CacheScope before starting this "
                                 "experiment.")},
                     {"requirements",
                      Json::array({"torch", "torch-mlir", "LLVM/MLIR 22"})}};
                 send(response, 200,
                      {{"templates", Json::array({std::move(item)})}});
               });
             });
  server.Post("/api/v1/experiments",
              [impl](const auto &request, auto &response,
                     const httplib::ContentReader &reader) {
                if (request.is_multipart_form_data())
                  impl->upload(request, response, reader);
                else
                  impl->create_template(request, response, reader);
              });
  server.Get(R"(/api/v1/experiments/([^/]+)/results)",
             [impl](const auto &request, auto &response) {
               impl->guarded(response, [&] {
                 fs::path path;
                 {
                   std::lock_guard lock(impl->mutex);
                   auto job = impl->find(request.matches[1]);
                   if (job->template_id.empty() || job->status != "completed")
                     throw RequestError(409,
                                        "Template results are not available");
                   path = job->directory / "output/ui-results.json";
                 }
                 send(response, 200, read(path));
               });
             });
  server.Get(R"(/api/v1/experiments/([^/]+)/artifacts)",
             [impl](const auto &request, auto &response) {
               impl->guarded(response, [&] {
                 std::lock_guard lock(impl->mutex);
                 auto job = impl->find(request.matches[1]);
                 if (job->template_id.empty() || job->status != "completed")
                   throw RequestError(409, "Artifacts are not available");
                 send(response, 200,
                      {{"experiment_id", job->id},
                       {"template_id", job->template_id},
                       {"artifacts", job->descriptor().at("artifacts")}});
               });
             });
  server.Get(
      R"(/api/v1/experiments/([^/]+)/artifacts/(report|summary|stepper))",
      [impl](const auto &request, auto &response) {
        impl->guarded(response, [&] {
          std::shared_ptr<Job> job;
          {
            std::lock_guard lock(impl->mutex);
            job = impl->find(request.matches[1]);
            if (job->template_id.empty() || job->status != "completed")
              throw RequestError(409, "Artifacts are not available");
          }
          const auto artifact = request.matches[2].str();
          const auto output = job->directory / "output";
          if (artifact == "report") {
            if (job->template_id == "local-torch-project") {
              response.set_redirect("/?view=report&experiment=" + job->id);
              return;
            }
            std::ifstream input(output / "report.html", std::ios::binary);
            if (!input)
              throw RequestError(404, "Report artifact was not found");
            response.status = 200;
            response.set_content(
                std::string(std::istreambuf_iterator<char>(input), {}),
                "text/html; charset=utf-8");
            return;
          }
          if (artifact == "summary") {
            send(response, 200, read(output / "summary.json"));
            return;
          }
          const auto summary = read(output / "summary.json");
          const bool project = job->template_id == "local-torch-project";
          std::string variant;
          fs::path root;
          std::string file_prefix;
          if (project) {
            if (!summary.contains("stepper_variants") ||
                !summary.at("stepper_variants").is_object() ||
                summary.at("stepper_variants").empty())
              throw RequestError(404, "Stepper artifacts were disabled");
            const auto &choices = summary.at("stepper_variants");
            variant = request.has_param("variant")
                          ? request.get_param_value("variant")
                          : choices.begin().key();
            if (!choices.contains(variant))
              throw RequestError(404, "Unknown Stepper schedule");
            root = output / "stepper" / variant;
            file_prefix = variant + "/";
          } else {
            if (summary.at("stepper_demo").is_null())
              throw RequestError(404, "Stepper artifact was disabled");
            variant = summary.at("stepper_demo").at("variant").get<std::string>();
            root = output / "stepper-demo";
          }
          Json files = Json::array();
          for (const auto &entry : fs::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file())
              continue;
            const auto relative = fs::relative(entry.path(), root);
            if (relative.filename() != "trace.json" &&
                relative.extension() != ".mlir")
              continue;
            const auto name = relative.generic_string();
            files.push_back(
                {{"name", name},
                 {"url", "/api/v1/experiments/" + job->id +
                             "/artifacts/stepper-file/" + file_prefix + name}});
          }
          Json levels = Json::array();
          const Json *source_levels = nullptr;
          Json profile = nullptr;
          Json hierarchy = Json::object();
          if (project && summary.contains("stepper_profiles")) {
            const auto &profiles = summary.at("stepper_profiles");
            const auto requested_profile = request.has_param("profile")
                                               ? request.get_param_value("profile")
                                               : profiles.begin().key();
            if (!profiles.contains(requested_profile))
              throw RequestError(404, "Unknown Stepper cache profile");
            profile = requested_profile;
            const auto &selected = profiles.at(requested_profile);
            if (selected.is_object() && selected.contains("levels")) {
              source_levels = &selected.at("levels");
              hierarchy = selected.value("hierarchy", Json::object());
            } else {
              // Compatibility with project artifacts created before hierarchy
              // became part of the Stepper manifest.
              source_levels = &selected;
            }
          } else if (summary.contains("stepper_levels")) {
            source_levels = &summary.at("stepper_levels");
          } else {
            source_levels = &summary.at("variants")
                                 .at(variant)
                                 .at("demo_cache")
                                 .at("levels");
          }
          for (const auto &level : *source_levels) {
            auto config = level.at("cache_config");
            config["name"] = level.at("name");
            config.erase("set_count");
            levels.push_back(std::move(config));
          }
          if (hierarchy.empty()) {
            Json links = Json::array();
            for (std::size_t index = 1; index < source_levels->size(); ++index)
              links.push_back("exclusive");
            hierarchy = {{"inclusion_policies", std::move(links)},
                         {"writeback_clean", false},
                         {"non_inclusive_fill", "all_levels"}};
          }
          send(response, 200,
               {{"experiment_id", job->id},
                {"variant", variant},
                {"profile", profile},
                {"hierarchy", hierarchy},
                {"levels", levels},
                {"files", files}});
        });
      });
  server.Get(
      R"(/api/v1/experiments/([^/]+)/artifacts/stepper-file/(.+))",
      [impl](const auto &request, auto &response) {
        impl->guarded(response, [&] {
          fs::path root;
          {
            std::lock_guard lock(impl->mutex);
            auto job = impl->find(request.matches[1]);
            if (job->template_id.empty() || job->status != "completed")
              throw RequestError(409, "Artifacts are not available");
            root = job->template_id == "local-torch-project"
                       ? job->directory / "output/stepper"
                       : job->directory / "output/stepper-demo";
          }
          const auto target = fs::weakly_canonical(
              root / fs::path(request.matches[2].str()));
          const auto relative = fs::relative(target, root);
          if (relative.empty() || *relative.begin() == ".." ||
              !fs::is_regular_file(target) ||
              (relative.filename() != "trace.json" &&
               relative.extension() != ".mlir"))
            throw RequestError(404, "Stepper artifact was not found");
          std::ifstream input(target, std::ios::binary);
          response.status = 200;
          response.set_content(
              std::string(std::istreambuf_iterator<char>(input), {}),
              relative.extension() == ".json" ? "application/json"
                                                : "text/plain; charset=utf-8");
        });
      });
  server.Get(R"(/api/v1/experiments(?:/([^/]+))?(?:/(report))?)",
             [impl](const auto &request, auto &response) {
               impl->guarded(response, [&] {
                 std::shared_ptr<Job> job;
                 {
                   std::lock_guard lock(impl->mutex);
                   const auto id = request.matches[1].str();
                   if (id.empty()) {
                     Json jobs = Json::array();
                     if (impl->current)
                       jobs.push_back(impl->current->descriptor());
                     send(response, 200, {{"experiments", jobs}});
                     return;
                   }
                   job = impl->find(id);
                   if (!request.matches[2].matched) {
                     send(response, 200, job->descriptor());
                     return;
                   }
                   if (job->status != "completed")
                     throw RequestError(409,
                                        "A complete report is not available");
                 }
                 send(response, 200, read(job->directory / "report.json"));
               });
             });
  server.Post(
      R"(/api/v1/experiments/([^/]+)/(run|cancel))",
      [impl](const auto &request, auto &response) {
        impl->guarded(response, [&] {
          impl->check_origin(request);
          std::lock_guard lock(impl->mutex);
          auto job = impl->find(request.matches[1]);
          if (request.matches[2] == "cancel") {
            if (job->active()) {
              job->cancelled = true;
              job->status = "cancelling";
            }
            send(response, 202, job->descriptor());
            return;
          }
          if (job->status != "ready")
            throw RequestError(409,
                               "Experiment is not ready or has already run");
          require(json_content_type(request.get_header_value("Content-Type")),
                  "Expected application/json");
          const auto spec = parse_api_json(request.body);
          validate(spec, job->metadata);
          {
            std::ofstream output(job->directory / "spec.json");
            output << spec;
            if (!output.good())
              throw std::runtime_error("Cannot save experiment parameters");
          }
          job->spec = spec;
          impl->start(job, "run");
          send(response, 202, job->descriptor());
        });
      });
}
Experiments::~Experiments() = default;
} // namespace cache_simulator
