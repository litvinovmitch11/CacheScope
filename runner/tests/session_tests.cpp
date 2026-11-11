#include "runner_api.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace cache_simulator;
using Json = nlohmann::json;

namespace {
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct ClearGate {
  std::atomic<bool> paused{false}, entered{false}, release{false};
};

// Fail after a real insertion, so the failed command actually left cache state
// partially changed. The next independent run must still start cold.
class FailingCache final : public ICacheLevel {
public:
  FailingCache(const CacheConfig &config,
               const ReplacementPolicyFactory &policy, std::atomic<bool> &fail,
               ClearGate &gate)
      : cache_(
            {config.size_bytes, config.line_size_bytes, config.associativity},
            policy),
        fail_(fail), gate_(gate) {}
  LookupResult lookup(std::uint64_t address) const override {
    return cache_.lookup(address);
  }
  void touch(std::uint64_t address, AccessType type) override {
    cache_.touch(address, type);
  }
  std::optional<CacheLine> invalidate(std::uint64_t address) override {
    return cache_.invalidate(address);
  }
  InsertResult insert(CacheLine line) override {
    auto result = cache_.insert(line);
    if (++insertions_ == 2 && fail_.exchange(false))
      throw std::runtime_error("injected failure after cache insertion");
    return result;
  }
  void clear() override {
    if (gate_.paused) {
      gate_.entered = true;
      while (!gate_.release)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    cache_.clear();
  }

private:
  SetAssociativeCacheLevel cache_;
  std::atomic<bool> &fail_;
  std::size_t insertions_ = 0;
  ClearGate &gate_;
};

struct TemporaryTrace {
  std::filesystem::path path;
  TemporaryTrace() {
    auto pattern =
        (std::filesystem::temp_directory_path() / "cachescope-failure-XXXXXX")
            .string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    require(fd >= 0, "create test trace");
    close(fd);
    path = name.data();
  }
  ~TemporaryTrace() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};
} // namespace

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  try {
    std::ifstream fixture(argv[1]);
    Json trace;
    fixture >> trace;
    auto second = trace.at("events").at(0);
    second["sequence"] = 1;
    second["byte_offset"] = 64;
    trace["events"].push_back(second);
    trace["total_events"] = trace["recorded_events"] = 2;
    TemporaryTrace input;
    {
      std::ofstream output(input.path);
      output << trace;
      require(output.good(), "write test trace");
    }
    std::atomic<bool> fail{true};
    ClearGate gate;
    RunnerApi api(
        [&](const CacheConfig &config, const ReplacementPolicyFactory &policy) {
          return std::make_unique<FailingCache>(config, policy, fail, gate);
        });
    const Json create{{"trace_path", input.path.string()},
                      {"levels",
                       {{{"name", "L1"},
                         {"size_bytes", 128},
                         {"line_size_bytes", 64},
                         {"associativity", 2},
                         {"replacement_policy", "LRU"}}}}};
    const auto created = api.handle("POST", "/api/v1/sessions", {}, create);
    require(created.status == 202, "session accepted");
    const auto path =
        "/api/v1/sessions/" + created.body.at("session_id").get<std::string>();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
      const auto status = api.handle("GET", path, {}, nullptr);
      if (status.body.at("status") != "preparing") {
        require(status.body.at("status") == "ready", "session prepared");
        break;
      }
      require(std::chrono::steady_clock::now() < deadline,
              "preparation timeout");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto response = api.handle("POST", path + "/step", {},
                               {{"expected_revision", "1"}, {"count", 2}});
    require(response.status == 500 &&
                response.body.at("error").at("code") == "execution_failed",
            "failure surfaced");
    require(response.body.at("error").at("recovery") == "reset",
            "reset recovery advertised");
    const auto status = api.handle("GET", path, {}, nullptr).body;
    require(status.at("status") == "failed" && status.at("cursor") == "1" &&
                status.at("revision") == "2",
            "failed prefix is published");
    response = api.handle(
        "GET", path + "/state",
        {{"expected_revision", "2"}, {"level", "1"}, {"set_limit", "1"}},
        nullptr);
    require(response.status == 409 &&
                response.body.at("error").at("code") == "session_failed",
            "partial state not exposed");
    response =
        api.handle("POST", path + "/step", {}, {{"expected_revision", "2"}});
    require(response.status == 409, "failed replay cannot continue");
    gate.paused = true;
    auto resetting = std::async(std::launch::async, [&] {
      return api.handle("POST", path + "/reset", {},
                        {{"expected_revision", "2"}});
    });
    const auto reset_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!gate.entered && std::chrono::steady_clock::now() < reset_deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto during_reset = api.handle("GET", path, {}, nullptr);
    const auto blocked = api.handle(
        "GET", path + "/state",
        {{"expected_revision", "2"}, {"level", "1"}, {"set_limit", "1"}},
        nullptr);
    gate.release = true;
    response = resetting.get();
    require(gate.entered && during_reset.body.at("busy") == true &&
                during_reset.body.at("status") == "failed",
            "failed reset advertises busy with the last published state");
    require(during_reset.body.at("revision") == "2" &&
                during_reset.body.at("cursor") == "1",
            "reset does not publish an intermediate revision or cursor");
    require(blocked.status == 409 &&
                blocked.body.at("error").at("code") == "session_busy",
            "snapshot blocked during failed reset");
    std::cout << during_reset.body.dump() << '\n';
    require(response.status == 200 && response.body.at("cursor") == "0" &&
                response.body.at("revision") == "3",
            "reset recovers");
    response = api.handle(
        "GET", path + "/state",
        {{"expected_revision", "3"}, {"level", "1"}, {"set_limit", "1"}},
        nullptr);
    require(response.status == 200, "cold snapshot available");
    for (const auto &cell :
         response.body.at("cache").at("sets").at(0).at("ways"))
      require(cell.at("valid") == false && cell.at("dirty") == false,
              "partial insertion cleared");
    response = api.handle("POST", path + "/step", {},
                          {{"expected_revision", "3"}, {"count", 2}});
    require(response.status == 200 && response.body.at("cursor") == "2",
            "replay works after reset");
    require(response.body.at("summary").at("totals").at("misses") == "2" &&
                response.body.at("summary").at("memory").at("reads") == "2",
            "new run is cold with clean statistics");
    std::cout << "session failure/reset integration OK\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
