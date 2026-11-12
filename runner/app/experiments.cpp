#include "experiments.hpp"
#include "http_json.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
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
      spec.is_object() && spec.size() == 5 && spec.contains("function") &&
          spec.contains("bindings") && spec.contains("levels") &&
          spec.contains("timeout_seconds") && spec.contains("trace_limit"),
      "Expected function, bindings, levels, timeout_seconds and trace_limit");
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
    require(policy == "LRU" || policy == "FIFO" || policy == "Random",
            "Unknown cache policy");
    require(level.size() == (level.contains("random_seed") ? 6 : 5),
            "Unknown cache configuration field");
    if (level.contains("random_seed")) {
      require(policy == "Random", "Only Random accepts random_seed");
      decimal(level.at("random_seed"), false);
    }
  }
}

struct Job {
  fs::path directory;
  std::string id, status = "preparing";
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
    Json phase = nullptr;
    try {
      phase = read(directory / "progress.json").at("stage");
    } catch (const std::exception &) {
    }
    if (status == "ready" || status == "completed")
      phase = nullptr;
    return {{"experiment_id", id},  {"status", status}, {"stage", phase},
            {"metadata", metadata}, {"spec", spec},     {"error", error}};
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
          {"stage", mode == "prepare" ? "inspection" : "compilation"}};
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
    std::vector<std::string> arguments{
        "python3", (app / "compiler/experiment_worker.py").string(), mode,
        "--job", job->directory.string()};
    std::vector<char *> argv;
    for (auto &argument : arguments)
      argv.push_back(argument.data());
    argv.push_back(nullptr);
    const auto result = posix_spawnp(&job->pid, "python3", &actions,
                                     &attributes, argv.data(), environ);
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
      if (mode == "run" && !job->spec.at("timeout_seconds").is_null() &&
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
        else if (!fs::is_regular_file(job->directory / "report.json"))
          throw std::runtime_error("Worker produced no complete report");
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
  void upload(const httplib::Request &request, httplib::Response &response,
              const httplib::ContentReader &reader) {
    guarded(response, [&] {
      check_origin(request);
      require(request.is_multipart_form_data(),
              "Expected multipart MLIR and NPZ files");
      auto job = std::make_shared<Job>();
      {
        std::lock_guard lock(mutex);
        if (current && current->active())
          throw RequestError(409, "An experiment is already active");
        std::string pattern =
            (fs::temp_directory_path() / "cachescope-experiment-XXXXXX")
                .string();
        if (!mkdtemp(pattern.data()))
          throw std::runtime_error("Cannot create experiment directory");
        job->directory = pattern;
        job->id = job->directory.filename().string();
        current = job;
      }
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
  server.Post("/api/v1/experiments",
              [impl](const auto &request, auto &response,
                     const httplib::ContentReader &reader) {
                impl->upload(request, response, reader);
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
